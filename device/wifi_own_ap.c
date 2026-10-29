/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0
 * Copyright (C) 2026 Henri David */
/* MODE AP on fm1-base's own Wi-Fi stack (fm1.toml wifi_ap = true: FM1B_WIFI_AP): the FM-1 is the access point. From
 * Optimist's wifi/wifi_apd.c, which has run on an FM-1 (a phone joined, did the 4-way handshake, got an address, sent
 * data and was a Link peer). Part of device/wifi_own.c (the unity build), after the station's service: wa_poll runs once
 * a main-loop pass beside wc_poll; while the access point is up the service (wl) runs as the access point (wl.ap: wl_poll
 * hands each frame to wa_rx, wl_send each frame out to wa_send), and Link (link = true) runs on it as on a station.
 *
 * The pure parts are wifi/wifi_ap.c (the beacon, the management answers, the authenticator) and wifi/wifi_ap_dhcp.c (the
 * clients' addresses); this is the radio:
 *   - a beacon every 102 ms (no ACK wanted); probe, authentication and association answered from wa_rx;
 *   - one handshake record per client (hs[i]), M1 20 ms after the association answer, M1 / M3 again after 1 s with no
 *     answer, the client let go (deauthentication, reason 15) after WA_HS_TRIES sends;
 *   - each client's pairwise key in the MAC at WCID i + 1 (WCID 0 is the MAC's drop entry: never used);
 *   - what is not protected data (beacons, management, EAPOL) goes out at WA_PLAIN, a WCID never keyed: the MAC encrypts
 *     whatever it sends at a keyed WCID, whatever the frame's own Protected bit says;
 *   - a group-addressed frame (Link's multicast) goes once to each keyed client, as a unicast frame with the group
 *     address left in its IP header (the pairwise path is the one proven on this MAC);
 *   - data from a station that is not associated (it was before a restart): told to join again (deauthentication,
 *     reason 7), once a second at most;
 *   - a ping to the access point is answered (wifi_ip.c, as an access point only).
 * The access point is 192.168.4.1, its clients 192.168.4.2 to .5. Its network (the name, the channel, the WPA2 key made
 * from the password, never the password) is the record's (wifi_nets.h struct wn_apcfg), kept with the networks:
 *   MODE AP      (WIFI_MODE_AP, "AP AUTO") runs whenever a key is kept, from power-on
 *   MODE AP_MAN  (WIFI_MODE_AP_MAN, "AP MAN") the same, but started by plat_wifi_ap_run(1) or a password set, and off at
 *                power-on
 * plat_wifi_ap_start runs one that is not kept, with MODE OFF. The station's manager rests in either AP mode. */
#define WA_PLAIN 8u                                     /* the WCID of the frames sent in the clear: never keyed */
#define WA_BEACON_MS 102u
#define WA_M1_DELAY_MS 20u                              /* association answered -> M1 (the client needs a moment) */
#define WA_HS_RETRY_MS 1000u
#define WA_HS_TRIES 4u
#define WA_IP 0xC0A80401u                               /* 192.168.4.1 */
#define WA_CH_DEFAULT 6u
#define WA_RESTART_MS 1200u                             /* a channel changed: the access point restarts once it rests this long */
#define WA_STRAY_MS 1000u
enum { WA_OFF, WA_PMK, WA_RADIO, WA_UP, WA_FAIL };
_Static_assert(WIFI_AP_CLIENTS < WA_PLAIN, "the clients' WCIDs 1..4 below WA_PLAIN");

static struct {
    uint8_t st, ch, ssid_len;
    uint8_t ssid[WN_SSID_MAX];
    uint8_t pmk[32];
    uint8_t user_on;                                    /* AP MAN: started (plat_wifi_ap_run, a password set); never kept */
    uint8_t by_mode;                                    /* started by the mode (the record's network), else plat_wifi_ap_start */
    uint8_t save_key;                                   /* the key job in hand is a new password: kept when done */
    uint8_t keyed[WIFI_AP_CLIENTS];                     /* the client's keys are in the MAC */
    uint8_t tries[WIFI_AP_CLIENTS];                     /* handshake sends */
    uint32_t t_due[WIFI_AP_CLIENTS];                    /* ms of the next handshake send (0: none due) */
    uint32_t t_restart, t_beacon, t_stray;
    char pass[64];                                      /* until the key job has it */
    struct wc_pmk_job job;
    struct wifi_ap ap;                                  /* the network: the association table, the group key */
    struct wifi_ap hs[WIFI_AP_CLIENTS];                 /* each client's handshake: ANonce, PTK, replay counter */
    struct wifi_ap_dhcp dhcp;
    struct { uint32_t beacon, assoc, m2, m4, bad, data, dhcp, stray, tx, tx_err; } n;
} wa PLAT_POOL;                                         /* (the CPU's only: the frames are built in wifi_txb) */

static void wa_tx(uint32_t len, uint32_t wcid, uint32_t ack)   /* wifi_txb's frame out at that WCID */
{
    fm1_wifi_tx_prepare(wifi_txb, len, wcid, ack);
    if (fm1_wifi_tx(wifi_txb) == 0)
        wa.n.tx++;
    else
        wa.n.tx_err++;
}
static int wa_slot_of(const uint8_t mac[6])             /* the client's table slot, or -1 */
{
    const struct wifi_ap_client *cl = wifi_ap_find_client(&wa.ap, mac);
    return cl ? (int)(cl - wa.ap.clients) : -1;
}

/* a frame wifi_txb holds (the IP layer's, the handshake's, Link's), out at the right WCID */
static void wa_send(uint32_t len)
{
    uint8_t *f = own_frame();
    uint32_t i;
    int s;
    if (!(f[0] == 0x08u && (f[1] & 0x40u))) {           /* not protected data: beacons, management, EAPOL */
        wa_tx(len, WA_PLAIN, (f[4] & 1u) ? 0u : 1u);
        return;
    }
    if (f[4] & 1u) {                                    /* group-addressed: a copy to each keyed client */
        for (i = 0; i < WIFI_AP_CLIENTS; i++) {
            if (!wa.keyed[i])
                continue;
            memcpy(f + 4, wa.ap.clients[i].mac, 6);
            wa_tx(len, i + 1u, 1u);
        }
        return;
    }
    s = wa_slot_of(f + 4);
    if (s >= 0 && wa.keyed[s])
        wa_tx(len, (uint32_t)s + 1u, 1u);
}

/* ---- the MAC's key tables, per client */
static void wa_key_clear(uint32_t w)
{
    fm1_wifi_mac_wr(FM1_WIFI_WCID_ATTR(w), 0);
    fm1_wifi_mac_wr(FM1_WIFI_IVEIV(w), 0);
    fm1_wifi_mac_wr(FM1_WIFI_IVEIV(w) + 4u, 0);
    fm1_wifi_key_wr(FM1_WIFI_PKEY(w), 0);
    fm1_wifi_mac_wr(FM1_WIFI_WCID_ADDR(w), 0);
    fm1_wifi_mac_wr(FM1_WIFI_WCID_ADDR(w) + 4u, 0);
}
static void wa_key_install(uint32_t w, const uint8_t mac[6], const uint8_t tk[16])
{
    fm1_wifi_addr_wr(FM1_WIFI_WCID_ADDR(w), mac, 0);
    fm1_wifi_key_wr(FM1_WIFI_PKEY(w), tk);
    fm1_wifi_mac_wr(FM1_WIFI_WCID_ATTR(w), FM1_WIFI_CIPHER_CCMP << 1 | 1u);   /* CCMP, the pairwise table */
    fm1_wifi_mac_wr(FM1_WIFI_IVEIV(w), 0x20000001u);    /* IV: PN 1, ExtIV, key id 0; EIV 0 */
    fm1_wifi_mac_wr(FM1_WIFI_IVEIV(w) + 4u, 0);
}
static void wa_group_install(void)                      /* the group key in the shared table, BSS 0 */
{
    uint32_t id = wa.ap.gtk_id & 3u;
    fm1_wifi_key_wr(FM1_WIFI_SKEY(id), wa.ap.gtk);
    fm1_wifi_mac_wr(FM1_WIFI_SKEY_MODE, FM1_WIFI_CIPHER_CCMP << (id * 4u));
}
static void wa_client_drop(uint32_t i)                  /* its keys out of the MAC, its handshake over */
{
    wa.keyed[i] = 0;
    wa.t_due[i] = 0;
    wa.tries[i] = 0;
    wa_key_clear(i + 1u);
}
static void wa_deauth(const uint8_t mac[6], uint16_t reason)
{
    wa_tx(wifi_ap_build_deauth(&wa.ap, mac, reason, own_frame()), WA_PLAIN, 1u);
}

/* ---- DHCP: the datagram the IP layer hands over (port 67); the reply to the client's own address */
static uint32_t wa_udp_srv(struct wifi_ip *wip, const uint8_t *src_mac, const uint8_t *msg, uint32_t n, uint8_t *tx_buf)
{
    uint8_t rep[WIFI_AP_DHCP_MSG_MAX];
    uint32_t rl, yi;
    (void)src_mac;
    rl = wifi_ap_dhcp_reply(&wa.dhcp, msg, n, rep);
    if (!rl)
        return 0;
    wa.n.dhcp++;
    yi = (uint32_t)rep[16] << 24 | (uint32_t)rep[17] << 16 | (uint32_t)rep[18] << 8 | rep[19];
    if (yi)
        wifi_ip_arp_learn(wip, yi, rep + 28);           /* (chaddr: where unicast to that address goes) */
    return wifi_ip_reply_udp(wip, rep + 28, 0xFFFFFFFFu, WIFI_IP_DHCP_PORT_SERVER, WIFI_IP_DHCP_PORT_CLIENT, rep, rl, tx_buf);
}

/* ---- the handshake, one record per client */
static void wa_start_hs(uint32_t i, const uint8_t mac[6])   /* the association was answered: M1 soon */
{
    if (wa.keyed[i])
        wa_client_drop(i);
    wa.hs[i] = wa.ap;                                   /* (the shared record: the key, the group key, the name) */
    wifi_ap_start_client(&wa.hs[i], mac);
    wa.tries[i] = 0;
    wa.t_due[i] = own_ms() + WA_M1_DELAY_MS;
    if (!wa.t_due[i])
        wa.t_due[i] = 1u;
}
static void wa_send_eapol(uint32_t i, const uint8_t *eapol, uint32_t n)
{
    wa_send(wifi_ap_encap_eapol(&wa.ap, wa.ap.clients[i].mac, eapol, n, own_frame()));
}
static void wa_hs_step(uint32_t i, uint32_t now)        /* M1 / M3 due: sent, or the client given up */
{
    uint8_t eb[WIFI_AP_FRAME_MAX];
    struct wifi_ap *h = &wa.hs[i];
    if (!wa.t_due[i] || (int32_t)(now - wa.t_due[i]) < 0)
        return;
    if (wa.keyed[i] || wa.ap.clients[i].state != WIFI_AP_CL_ASSOC) {
        wa.t_due[i] = 0;
        return;
    }
    if (++wa.tries[i] > WA_HS_TRIES) {                  /* the 4-way timed out: reason 15 */
        wa_deauth(wa.ap.clients[i].mac, 15u);
        memset(&wa.ap.clients[i], 0, sizeof wa.ap.clients[i]);
        wa_client_drop(i);
        return;
    }
    wa_send_eapol(i, eb, (h->state == WIFI_AP_STATE_WAIT_M4 || h->state == WIFI_AP_STATE_PTK) ? wifi_ap_build_m3(h, eb)
                                                                                               : wifi_ap_build_m1(h, eb));
    wa.t_due[i] = now + WA_HS_RETRY_MS;
}
static void wa_on_eapol(uint32_t i, const uint8_t *pkt, uint32_t n)
{
    uint8_t eb[WIFI_AP_FRAME_MAX];
    struct wifi_ap *h = &wa.hs[i];
    if (h->state == WIFI_AP_STATE_WAIT_M2) {
        if (wifi_ap_rx_m2(h, pkt, n) != 0) {
            wa.n.bad++;
            return;
        }
        wa.n.m2++;
        wa_send_eapol(i, eb, wifi_ap_build_m3(h, eb));
        wa.tries[i] = 1;
        wa.t_due[i] = own_ms() + WA_HS_RETRY_MS;
    } else if (h->state == WIFI_AP_STATE_WAIT_M4) {
        if (wifi_ap_rx_m4(h, pkt, n) != 0) {
            wa.n.bad++;
            return;
        }
        wa.n.m4++;
        wa_key_install(i + 1u, wa.ap.clients[i].mac, h->ptk.tk);
        wa.keyed[i] = 1;
        wa.t_due[i] = 0;
    }
}

static void wa_rx_mgmt(const uint8_t *f, uint32_t len, int64_t now)
{
    uint8_t *txf = own_frame(), sub = f[0] & 0xFCu;
    uint32_t tx_len = 0;
    int s, r;
    if (sub == 0x80u || sub == 0x50u)                   /* beacons, probe answers: the neighbours' */
        return;
    s = wa_slot_of(f + 10);
    if (s >= 0 && wa.keyed[s] && (sub == 0xB0u || sub == 0x00u))
        wa_client_drop((uint32_t)s);                    /* it starts over: the keys it held are gone */
    r = wifi_ap_rx_mgmt(&wa.ap, f, len, (uint64_t)now, txf, &tx_len);
    if (sub == 0xA0u || sub == 0xC0u) {                 /* it left */
        if (s >= 0)
            wa_client_drop((uint32_t)s);
        return;
    }
    if (r != 1)
        return;
    wa_send(tx_len);
    if (sub == 0x00u && txf[0] == 0x10u && txf[26] == 0 && txf[27] == 0) {   /* association accepted */
        s = wa_slot_of(f + 10);
        if (s >= 0) {
            wa.n.assoc++;
            wa_start_hs((uint32_t)s, f + 10);
        }
    }
}
/* one 802.11 frame the radio heard (the receive header stripped) */
static void wa_rx(const uint8_t *f, uint32_t len, int8_t rssi, int64_t now)
{
    uint8_t *txf = own_frame();
    uint32_t tx_len = 0, hl, el;
    const uint8_t *e;
    int s;
    (void)rssi;
    wl.n_rx++;
    if (len < 24u)
        return;
    if ((f[0] & 0x0Cu) == 0u) {
        wa_rx_mgmt(f, len, now);
        return;
    }
    if ((f[0] & 0x0Cu) != 0x08u || (f[1] & 0x03u) != 0x01u || memcmp(f + 4, wa.ap.mac, 6) != 0)
        return;                                         /* (data frames to us from a station only) */
    wa.n.data++;
    hl = (f[0] & 0x80u) ? 26u : 24u;
    s = wa_slot_of(f + 10);
    e = wifi_ap_unwrap_eapol(&wa.ap, f, len, &el);
    if (e) {
        if (s >= 0)
            wa_on_eapol((uint32_t)s, e, el);
        return;
    }
    if (s < 0) {                                        /* a station we do not know (associated before a restart): told */
        if (own_ms() - wa.t_stray >= WA_STRAY_MS && !(f[10] & 1u)) {
            wa.t_stray = own_ms();
            wa.n.stray++;
            wa_deauth(f + 10, 7u);
        }
        return;
    }
    if (!wa.keyed[s] || len < hl + 8u || !wifi_ip_early_filter(&wl.ip, f, len))
        return;                                         /* (only a client whose keys are in) */
    wl.n_kept++;
    if (f[hl + 6u] == 0x08u && f[hl + 7u] == 0x00u && len >= hl + 8u + 20u) {   /* IPv4: where its source is */
        const uint8_t *ip = f + hl + 8u;
        wifi_ip_arp_learn(&wl.ip, (uint32_t)ip[12] << 24 | (uint32_t)ip[13] << 16 | (uint32_t)ip[14] << 8 | ip[15], f + 10);
    }
    if (wifi_ip_rx_frame(&wl.ip, f, len, now, txf, &tx_len) == 1 && tx_len)
        wa_send(tx_len);
}

/* ---- start and stop */
static void wa_bring_up(void)
{
    static const uint8_t SRV[4] = {192, 168, 4, 1};
    uint8_t mac[6];
    uint32_t i;
    own_mac(mac);
    wifi_sta_set_mac_regs(mac, mac, wa.ch);             /* our address, the BSSID the same, the channel, every frame in */
    wifi_sta_clear_hw_keys();
    for (i = 1; i <= WIFI_AP_CLIENTS; i++)
        wa_key_clear(i);
    wa_key_clear(WA_PLAIN);
    wifi_ap_init(&wa.ap, mac, wa.pmk, own_rand);
    wifi_ap_set_net(&wa.ap, wa.ssid, wa.ssid_len, wa.ch);
    wa_group_install();
    wifi_ap_dhcp_init(&wa.dhcp, SRV);
    for (i = 0; i < WIFI_AP_CLIENTS; i++)
        wa.keyed[i] = wa.tries[i] = 0, wa.t_due[i] = 0;
    wl_start_ap(mac, WA_IP);
    wa.t_beacon = own_ms();
    wa.st = WA_UP;
}
static void wa_stop(void)                               /* everyone told, the keys cleared, the service off */
{
    uint32_t i;
    if (wa.st == WA_UP) {
        for (i = 0; i < WIFI_AP_CLIENTS; i++)
            if (wa.ap.clients[i].state != WIFI_AP_CL_NONE) {
                wa_key_clear(i + 1u);                   /* (cleared first: the goodbye goes out plain) */
                wa_deauth(wa.ap.clients[i].mac, 3u);
            }
        memset(wa.keyed, 0, sizeof wa.keyed);
        wifi_sta_clear_hw_keys();
        wl.ap = 0;
        wl_stop();
    }
    memset(wa.pass, 0, sizeof wa.pass);
    memset(&wa.job, 0, sizeof wa.job);
    wa.st = WA_OFF;
}

/* ---- the mode's network (the record's wn_st.ap) */
static uint32_t wa_name(uint8_t *out)                   /* the name chosen, else FM1- and the radio address's last 4 digits */
{
    static const char H[] = "0123456789ABCDEF";
    uint8_t mac[6];
    if (wn_st.ap.ssid_len && wn_st.ap.ssid_len <= WN_SSID_MAX) {
        memcpy(out, wn_st.ap.ssid, wn_st.ap.ssid_len);
        return wn_st.ap.ssid_len;
    }
    own_mac(mac);
    out[0] = 'F', out[1] = 'M', out[2] = '1', out[3] = '-';
    out[4] = (uint8_t)H[mac[4] >> 4], out[5] = (uint8_t)H[mac[4] & 15u];
    out[6] = (uint8_t)H[mac[5] >> 4], out[7] = (uint8_t)H[mac[5] & 15u];
    return 8u;
}
static uint32_t wa_channel(void) { return wn_st.ap.ch ? wn_st.ap.ch : WA_CH_DEFAULT; }   /* (the record's: the AP may
                                                                                          * still be on the one before) */
static int wa_mode_ap(void) { return wn_st.mode == WN_MODE_AP || wn_st.mode == WN_MODE_AP_MAN; }
static int wa_wanted(void) { return wn_st.mode == WN_MODE_AP || (wn_st.mode == WN_MODE_AP_MAN && wa.user_on); }
static void wa_begin_mode(void)                         /* the record's network: no key job, the radio next */
{
    wa.by_mode = 1;
    wa.save_key = 0;
    wa.ssid_len = (uint8_t)wa_name(wa.ssid);
    wa.ch = (uint8_t)wa_channel();
    memcpy(wa.pmk, wn_st.ap.pmk, 32);
    wa.st = WA_RADIO;
}
static void wa_pmk_start(const char *pass, uint32_t pl)   /* the key from the password, a slice a pass */
{
    uint32_t i;
    for (i = 0; i < pl; i++)
        wa.pass[i] = pass[i];
    wa.pass[i] = 0;
    wc_pmk_begin(&wa.job, wa.pass, wa.ssid, wa.ssid_len);
    wa.st = WA_PMK;
}
static uint32_t wa_pass_len(const char *pass)           /* a WPA2 passphrase: 8 to 63 printable characters, else 0 */
{
    uint32_t n = 0;
    while (pass && pass[n] && n < 64u) {
        if (pass[n] < 32 || pass[n] > 126)
            return 0;
        n++;
    }
    return n >= 8u && n <= 63u ? n : 0u;
}

static void wa_poll(void)                               /* once a main-loop pass: one step */
{
    uint32_t now = own_ms(), i;
    if (wn_st.mode != WN_MODE_AP_MAN)
        wa.user_on = 0;                                 /* (AP MAN's own switch is off once another mode is chosen) */
    if (wa.by_mode && !wa_wanted() && wa.st != WA_OFF && !(wa.st == WA_PMK && wa.save_key)) {   /* MODE left AP, or AP MAN
                                                                                                * was stopped (a new key is
                                                                                                * still made and kept) */
        wa_stop();
        wa.by_mode = (uint8_t)wa_mode_ap();
        return;
    }
    if (!wa.by_mode && wa.st != WA_OFF && wn_st.mode != WN_MODE_OFF) {   /* a start of one not kept: until a mode is chosen */
        wa_stop();
        return;
    }
    if (wa.t_restart && (int32_t)(now - wa.t_restart) >= 0) {   /* a channel changed and rested: again on the new one */
        wa.t_restart = 0;
        if (wa.by_mode && wa.st != WA_OFF) {
            wa_stop();
            wa.by_mode = 1;
        }
    }
    switch (wa.st) {
    case WA_OFF:
        if (wa_wanted() && wn_st.ap.set == 1u)
            wa_begin_mode();
        break;
    case WA_PMK:
        if (wc_pmk_step(&wa.job, 256u, wa.pmk)) {
            memset(wa.pass, 0, sizeof wa.pass);
            if (wa.save_key) {                          /* a new password: its key kept (the name, the channel as the record has them) */
                (void)wn_ap_put(&wn_st, wn_st.ap.ssid_len ? wn_st.ap.ssid : 0, wn_st.ap.ssid_len, wn_st.ap.ch, wa.pmk);
                own_store_changed();
                wa.save_key = 0;
            }
            wa.st = wa_wanted() || !wa.by_mode ? WA_RADIO : WA_OFF;
        }
        break;
    case WA_RADIO: {
        int r;
        if (now < WC_RADIO_MS)
            break;                                      /* (the start-up's own work first, as the manager waits) */
        r = own_radio_up();
        if (r < 0)
            wa.st = WA_FAIL;
        else if (r > 0)
            wa_bring_up();
        break;
    }
    case WA_UP:
        if (now - wa.t_beacon >= WA_BEACON_MS) {
            wa.t_beacon += WA_BEACON_MS;
            if (now - wa.t_beacon >= 4u * WA_BEACON_MS)
                wa.t_beacon = now;                      /* (the main loop stalled: no burst of beacons to catch up) */
            wa_tx(wifi_ap_build_beacon(&wa.ap, (uint64_t)own_us(), own_frame()), WA_PLAIN, 0u);
            wa.n.beacon++;
        }
        for (i = 0; i < WIFI_AP_CLIENTS; i++)
            wa_hs_step(i, now);
        break;
    default:
        break;
    }
}

/* ---- plat.h */
int plat_wifi_ap_state(void)
{
    own_store_load();
    if (wa.st == WA_UP)
        return WIFI_AP_UP;
    if (wa.st == WA_FAIL)
        return WIFI_AP_FAILED;
    if (!wa.by_mode && wa.st != WA_OFF)
        return WIFI_AP_STARTING;
    if (!wa_mode_ap())
        return WIFI_AP_OFF;
    if (wa.st == WA_OFF && wn_st.ap.set != 1u)
        return WIFI_AP_NOKEY;
    if (wa.st == WA_OFF && !wa_wanted())
        return WIFI_AP_STOPPED;
    return WIFI_AP_STARTING;
}
int plat_wifi_ap_set(const uint8_t *ssid, uint32_t len, const char *pass)
{
    uint32_t pl = wa_pass_len(pass);
    own_store_load();
    if (len > WN_SSID_MAX || (len && !ssid) || !pl)
        return -1;
    wa_stop();
    if (wn_st.mode == WN_MODE_AP_MAN)
        wa.user_on = 1;                                 /* (a password typed starts it) */
    wa.by_mode = 1;
    wa.save_key = 1;
    if (len && ssid != wn_st.ap.ssid)                   /* (the name may be the record's own: a new password, the same name) */
        memcpy(wn_st.ap.ssid, ssid, len);
    wn_st.ap.ssid_len = (uint8_t)len;
    own_store_changed();
    wa.ssid_len = (uint8_t)wa_name(wa.ssid);
    wa.ch = (uint8_t)wa_channel();
    wa_pmk_start(pass, pl);
    return 0;
}
int plat_wifi_ap_set_channel(int ch)
{
    own_store_load();
    if (ch < 1 || ch > 13 || wn_ap_set_channel(&wn_st, (uint32_t)ch) != 0)
        return -1;
    own_store_changed();
    if (wa.by_mode && wa.st != WA_OFF) {
        wa.t_restart = own_ms() + WA_RESTART_MS;
        if (!wa.t_restart)
            wa.t_restart = 1u;
    }
    return 0;
}
int plat_wifi_ap_channel(void) { own_store_load(); return (int)(wa.st != WA_OFF && !wa.by_mode ? wa.ch : wa_channel()); }
int plat_wifi_ap_name(char *name)
{
    uint8_t b[WN_SSID_MAX];
    uint32_t n;
    own_store_load();
    n = wa.st != WA_OFF && !wa.by_mode ? wa.ssid_len : wa_name(b);
    if (name) {
        memcpy(name, wa.st != WA_OFF && !wa.by_mode ? wa.ssid : b, n);
        name[n] = 0;
    }
    return (int)n;
}
int plat_wifi_ap_has_key(void) { own_store_load(); return wn_st.ap.set == 1u; }
void plat_wifi_ap_forget(void)
{
    own_store_load();
    wn_ap_forget(&wn_st);
    own_store_changed();
    if (wa.by_mode)
        wa_stop();
}
int plat_wifi_ap_run(int on)
{
    own_store_load();
    if (wn_st.mode != WN_MODE_AP_MAN)
        return -1;
    if (on && wn_st.ap.set != 1u)
        return -2;
    wa.user_on = (uint8_t)(on != 0);
    return 0;
}
int plat_wifi_ap_start(const uint8_t *ssid, uint32_t len, const char *pass, int ch)
{
    uint32_t pl = wa_pass_len(pass);
    own_store_load();
    if (wn_st.mode != WN_MODE_OFF || len < 1u || len > WN_SSID_MAX || !ssid || !pl || ch < 1 || ch > 13)
        return -1;
    wa_stop();
    wc_enable(0);                                       /* (the station's manager steps aside) */
    memcpy(wa.ssid, ssid, len);
    wa.ssid_len = (uint8_t)len;
    wa.ch = (uint8_t)ch;
    wa.by_mode = wa.save_key = 0;
    memset(&wa.n, 0, sizeof wa.n);
    wa_pmk_start(pass, pl);
    return 0;
}
void plat_wifi_ap_stop(void)
{
    if (!wa.by_mode)
        wa_stop();
}
uint32_t plat_wifi_ap_clients(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < WIFI_AP_CLIENTS; i++)
        n += wa.keyed[i];
    return wa.st == WA_UP ? n : 0u;
}
int plat_wifi_ap_client(int i, uint8_t mac[6], uint32_t *ip)
{
    uint32_t k;
    if (wa.st != WA_UP || i < 0 || i >= (int)WIFI_AP_CLIENTS || !wa.keyed[i])
        return 0;
    if (mac)
        memcpy(mac, wa.ap.clients[i].mac, 6);
    if (ip) {
        *ip = 0;
        for (k = 0; k < WIFI_AP_DHCP_LEASES; k++)
            if (wa.dhcp.leases[k].used && !memcmp(wa.dhcp.leases[k].mac, wa.ap.clients[i].mac, 6))
                *ip = (WA_IP & 0xFFFFFF00u) | (k + 2u);
    }
    return 1;
}
