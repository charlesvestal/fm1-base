/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David */
/* The host simulator's radio and network for the own Wi-Fi stack (wifi_stack = "own"): device/wifi_own.c runs as on an
 * FM-1 over host/wifi_fake/fm1_wifi.h, and this is the air around it, all in this process (no socket, nothing of the
 * computer's own network):
 *   - one access point (the script's "wifi_net NAME PASS [CH]"): beacons every 100 ms on its channel, the probe /
 *     authentication / association answers and the WPA2 authenticator (wifi/wifi_ap.c: the other side of the FM-1's
 *     station), a DHCP server (wifi_ap_dhcp.c: 192.168.50.2.. for the FM-1) and ARP (wifi_ip.c as an access point);
 *   - with Link (FM1B_LINK) a Link peer on the access point's address, 192.168.50.1 (link/link.c, the port that
 *     matches Ableton's library to 0.001 ms of phase: Optimist's tools/link/interop.sh): "link_peer BPM", "link_peer
 *     tempo BPM", "link_peer play 0|1", "link_peer off".
 * With MODE AP (FM1B_WIFI_AP) a client of the FM-1's own access point too, as a phone would be ("wifi_client NAME
 * PASS"): it waits for the FM-1's beacon, authenticates, associates, does the 4-way handshake as the station
 * (wifi/wifi_sta.c), asks for an address (DHCP, wifi_ip.c as a station) and, with Link, is a Link peer on that address
 * ("client_peer BPM"); deauthenticated (an access point restarted), it joins again at the next beacon.
 * The FM-1 hears the network only on its channel. Nothing is encrypted (the MAC's CCMP is the hardware's): a frame with
 * the Protected bit goes by in the clear. Part of host/plat_host.c, after device/wifi_own.c. */
#define AIR_Q 32u
#define AIR_FRAME 640u
#define AIR_IP 0xC0A83201u                       /* 192.168.50.1: the access point, its DHCP server, the Link peer */
static struct {
    uint8_t on, ch, keyed;
    uint8_t name[WIFI_STA_SSID_MAX], name_len;
    uint8_t bssid[6];
    struct wifi_ap ap, hs;                        /* the network; the FM-1's handshake */
    struct wifi_ap_dhcp dhcp;
    struct wifi_ip ip;                            /* the access point's own side: ARP, DHCP's datagrams, the peer's UDP */
    uint32_t t_beacon;
    struct { uint8_t f[AIR_FRAME]; uint32_t n; } q[AIR_Q];   /* frames to the FM-1, in order */
    uint32_t qr, qw;
    uint32_t frames_in, frames_out, dhcp_acks;
    uint8_t txb[AIR_FRAME];
} air;
static int air_rssi = -45;                        /* the signal the FM-1 hears every frame at ("wifi_air rssi N"; a good one) */
static void air_queue(const uint8_t *f, uint32_t n)
{
    if (n && n <= AIR_FRAME && fm1_fake_ch == air.ch && air.qw - air.qr >= AIR_Q)
        fm1_fake_rx_over++;                       /* (the receive queue full: the MAC's overflow count) */
    if (!n || n > AIR_FRAME || air.qw - air.qr >= AIR_Q || fm1_fake_ch != air.ch)
        return;                                   /* (the FM-1 listens elsewhere, or its queue is full: lost, as on air) */
    memcpy(air.q[air.qw % AIR_Q].f, f, n);
    air.q[air.qw % AIR_Q].n = n;
    air.qw++;
    air.frames_out++;
}
static int32_t air_rx(uint32_t *buf, uint32_t max)   /* the RX DMA: length word (big endian), RXWI, the frame */
{
    uint8_t *b = (uint8_t *)buf;
    uint32_t n, k;
    if (air.qr == air.qw)
        return 0;
    k = air.qr++ % AIR_Q;
    n = FM1_WIFI_RXWI + air.q[k].n;
    if (n > max)
        return -1;
    b[0] = (uint8_t)(n >> 24), b[1] = (uint8_t)(n >> 16), b[2] = (uint8_t)(n >> 8), b[3] = (uint8_t)n;
    memset(b + 4, 0, FM1_WIFI_RXWI);
    b[4 + 2] = (uint8_t)air.q[k].n;
    b[4 + 3] = (uint8_t)(air.q[k].n >> 8 & 0x0Fu);
    b[4 + 8] = (uint8_t)(int8_t)air_rssi;
    memcpy(b + 4 + FM1_WIFI_RXWI, air.q[k].f, air.q[k].n);
    return (int32_t)n;
}

#if FM1B_LINK
static struct lk_node air_lk;
static uint8_t air_lk_on;
static int air_lk_send(void *ctx, uint32_t ip, uint16_t port, const uint8_t *p, uint32_t n)
{
    uint32_t len;
    (void)ctx;
    if (!air.on)
        return -1;
    len = wifi_ip_send_udp(&air.ip, LK_MCAST_PORT, ip, port, p, n, air.txb);
    air_queue(air.txb, len);
    return len ? 0 : -1;
}
static void air_lk_rx(void *ctx, int64_t now, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
                      const uint8_t *p, uint32_t n)
{
    (void)ctx, (void)dst_ip, (void)dst_port;
    if (air_lk_on)
        lk_rx(&air_lk, now, src_ip, src_port, p, n);
}
static void air_peer(int32_t mpb)                /* the peer on: a session of its own at that tempo */
{
    struct lk_cfg c;
    c.ip = AIR_IP;
    c.port = LK_MCAST_PORT;
    c.send = air_lk_send;
    c.ctx = 0;
    lk_init(&air_lk, &c, 0xA17u, mpb);
    lk_enable(&air_lk, 1, own_us());
    lk_set_addr(&air_lk, AIR_IP, LK_MCAST_PORT);
    air_lk_on = 1;
}
#endif

static uint32_t air_dhcp_srv(struct wifi_ip *wip, const uint8_t *src_mac, const uint8_t *msg, uint32_t n, uint8_t *tx_buf)
{
    uint8_t rep[WIFI_AP_DHCP_MSG_MAX];
    uint32_t rl, yi;
    (void)src_mac;
    rl = wifi_ap_dhcp_reply(&air.dhcp, msg, n, rep);
    if (!rl)
        return 0;
    yi = (uint32_t)rep[16] << 24 | (uint32_t)rep[17] << 16 | (uint32_t)rep[18] << 8 | rep[19];
    if (yi)
        wifi_ip_arp_learn(wip, yi, rep + 28);
    if (rep[242] == 5u)
        air.dhcp_acks++;
    return wifi_ip_reply_udp(wip, rep + 28, 0xFFFFFFFFu, WIFI_IP_DHCP_PORT_SERVER, WIFI_IP_DHCP_PORT_CLIENT, rep, rl, tx_buf);
}
static void air_net(const char *name, const char *pass, uint32_t ch)   /* the access point on (name 0: off) */
{
    static const uint8_t BSSID[6] = {0x02, 0xA1, 0x52, 0x00, 0x00, 0x01};
    static const uint8_t SRV[4] = {192, 168, 50, 1};
    uint8_t pmk[32];
    uint32_t n = 0;
    memset(&air, 0, sizeof air);
    if (!name)
        return;
    while (name[n] && n < WIFI_STA_SSID_MAX)
        n++;
    memcpy(air.name, name, n);
    air.name_len = (uint8_t)n;
    air.ch = (uint8_t)(ch >= 1u && ch <= 13u ? ch : 6u);
    memcpy(air.bssid, BSSID, 6);
    if (!strcmp(pass, "default")) {               /* an Optimist access point: its default password (wifi_ap_default_key) */
        char k[WIFI_AP_KEY_LEN + 1u];
        wifi_ap_default_key(air.name, n, k);
        wc_pmk(k, air.name, n, pmk);
    } else {
        wc_pmk(pass, air.name, n, pmk);
    }
    wifi_ap_init(&air.ap, air.bssid, pmk, own_rand);
    wifi_ap_set_net(&air.ap, air.name, n, air.ch);
    wifi_ap_dhcp_init(&air.dhcp, SRV);
    wifi_ip_ap_init(&air.ip, air.bssid, AIR_IP, own_rand);
    air.ip.udp_srv = air_dhcp_srv;
#if FM1B_LINK
    air.ip.udp_rx = air_lk_rx;
    (void)wifi_ip_bind(&air.ip, LK_MCAST_PORT);
#endif
    air.on = 1;
}
#if FM1B_WIFI_AP
#include "wifi_air_client.c"                     /* MODE AP: a client of the FM-1's access point */
#endif
static int air_tx(const uint8_t *f, uint32_t len, uint32_t wcid, uint32_t ack)   /* a frame the FM-1 sent */
{
    uint32_t tl = 0, el;
    const uint8_t *e;
    (void)wcid, (void)ack;
#if FM1B_WIFI_AP
    cli_hear(f, len);
#endif
    if (!air.on || fm1_fake_ch != air.ch || len < 24u)
        return 0;                                 /* (sent, nobody heard it) */
    air.frames_in++;
    if ((f[0] & 0x0Cu) == 0u) {                   /* management */
        uint8_t sub = f[0] & 0xFCu;
        if (sub == 0xC0u || sub == 0xA0u) {
            air.keyed = 0;
            return 0;
        }
        if (wifi_ap_rx_mgmt(&air.ap, f, len, (uint64_t)own_us(), air.txb, &tl) == 1) {
            air_queue(air.txb, tl);
            if (sub == 0x00u && air.txb[0] == 0x10u && air.txb[26] == 0 && air.txb[27] == 0) {   /* associated: M1 */
                uint8_t eb[WIFI_AP_FRAME_MAX];
                air.keyed = 0;
                air.hs = air.ap;
                wifi_ap_start_client(&air.hs, f + 10);
                air_queue(air.txb, wifi_ap_encap_eapol(&air.ap, f + 10, eb, wifi_ap_build_m1(&air.hs, eb), air.txb));
            }
        }
        return 0;
    }
    if ((f[0] & 0x0Cu) != 0x08u || (f[1] & 0x03u) != 0x01u || memcmp(f + 4, air.bssid, 6) != 0)
        return 0;
    e = wifi_ap_unwrap_eapol(&air.ap, f, len, &el);
    if (e) {
        uint8_t eb[WIFI_AP_FRAME_MAX];
        if (air.hs.state == WIFI_AP_STATE_WAIT_M2 && wifi_ap_rx_m2(&air.hs, e, el) == 0)
            air_queue(air.txb, wifi_ap_encap_eapol(&air.ap, f + 10, eb, wifi_ap_build_m3(&air.hs, eb), air.txb));
        else if (air.hs.state == WIFI_AP_STATE_WAIT_M4 && wifi_ap_rx_m4(&air.hs, e, el) == 0)
            air.keyed = 1;
        return 0;
    }
    if (!air.keyed)
        return 0;
    {
        uint32_t hl = (f[0] & 0x80u) ? 26u : 24u;
        if (len >= hl + 8u + 20u && f[hl + 6u] == 0x08u && f[hl + 7u] == 0x00u) {   /* IPv4: where its source is */
            const uint8_t *ip = f + hl + 8u;
            wifi_ip_arp_learn(&air.ip, (uint32_t)ip[12] << 24 | (uint32_t)ip[13] << 16 | (uint32_t)ip[14] << 8 | ip[15], f + 10);
        }
    }
    if (wifi_ip_rx_frame(&air.ip, f, len, own_us(), air.txb, &tl) == 1)
        air_queue(air.txb, tl);
    return 0;
}
static void air_tick(void)                        /* every simulated millisecond */
{
    uint32_t now = plat_ms();
    fm1_fake_tx = air_tx;
    fm1_fake_rx = air_rx;
    if (air.on && now - air.t_beacon >= 100u) {
        air.t_beacon = now;
        air_queue(air.txb, wifi_ap_build_beacon(&air.ap, (uint64_t)now * 1000u, air.txb));
    }
#if FM1B_LINK
    if (air_lk_on)
        lk_poll(&air_lk, own_us());
#endif
#if FM1B_WIFI_AP
    cli_tick();
#endif
}

/* a reboot (the script's): the stack as RAM would be after a restart (the radio off; what is kept is in the store) */
static void wifi_own_forget(void)
{
    memset(&wc, 0, sizeof wc);
    memset(&wc_req, 0, sizeof wc_req);
    memset(&wl, 0, sizeof wl);
    memset(own_sock, 0, sizeof own_sock);
    wn_scan_clear(&wc_tab);
    own_group = 0;
    own_started = own_store_ok = own_store_dirty = 0;
#if FM1B_WIFI_AP
    memset(&wa, 0, sizeof wa);
#endif
    wn_stp = &own_store;
    fm1_fake_up = 0;
    memset(&wt, 0, sizeof wt);                    /* (the tuning's radio side: as at power-on) */
    fm1_fake_agc = FM1_WIFI_AGC_START;
#if FM1B_LINK
    memset(&own_lk, 0, sizeof own_lk);
    lko.want = lko.on = 0;
    lko.mpb = 500000;
#endif
}

/* the script's commands (host/plat_host.c): 1 = handled */
static int air_cmd(const char *cmd, const char *a, const char *b, const char *c)
{
    if (!strcmp(cmd, "wifi_net")) {               /* wifi_net NAME PASS [CH] | wifi_net off */
        if (!strcmp(a, "off"))
            air_net(0, 0, 0);
        else
            air_net(a, b, c[0] ? (uint32_t)atoi(c) : 6u);
        return 1;
    }
    if (!strcmp(cmd, "wifi_air")) {               /* wifi_air rssi DBM | wifi_air lost PCT | wifi_air retry PCT: the air's
                                                   * quality (the FM-1's frames never / only after a retry acknowledged) */
        if (!strcmp(a, "rssi"))
            air_rssi = atoi(b);
        else if (!strcmp(a, "lost"))
            fm1_fake_lost_pct = (uint32_t)atoi(b);
        else if (!strcmp(a, "retry"))
            fm1_fake_retry_pct = (uint32_t)atoi(b);
        else if (!strcmp(a, "lost_fast"))      /* (frames sent faster than 1 Mb/s only: the tuning's (f)) */
            fm1_fake_lost_fast_pct = (uint32_t)atoi(b);
        return 1;
    }
    if (!strcmp(cmd, "wifi") && !strcmp(a, "tune")) {   /* wifi tune MASK: the tuning's switches (1 cal, 2 filter, 4 agc, 8 rate) */
        plat_wifi_tune_set((uint32_t)atoi(b));
        return 1;
    }
    if (!strcmp(cmd, "wifi")) {                   /* wifi mode OFF|AUTO|MAN | wifi join NAME [PASS] | wifi leave | wifi scan */
        static const char *const M[] = {"OFF", "AUTO", "MAN", "AP", "AP_MAN"};
        int i;
        if (!strcmp(a, "mode")) {
            for (i = 0; i < (FM1B_WIFI_AP ? 5 : 3); i++)
                if (!strcmp(b, M[i]))
                    plat_wifi_mode(i);
            if (!strcmp(b, "JAM"))
                plat_wifi_mode(WIFI_MODE_JAM);
        } else if (!strcmp(a, "join")) {
            printf("  wifi join %s: %d\n", b, plat_wifi_join(b, c[0] ? c : 0));
        } else if (!strcmp(a, "leave")) {
            plat_wifi_leave();
        } else if (!strcmp(a, "scan")) {
            plat_wifi_scan();
        }
#if FM1B_WIFI_AP
        else
            cli_wifi_cmd(a, b, c);
#endif
        return 1;
    }
#if FM1B_WIFI_AP
    if (cli_cmd(cmd, a, b, c))
        return 1;
#endif
#if FM1B_LINK
    if (!strcmp(cmd, "link")) {                   /* link on | link off | link tempo BPM | link play 0|1 (the FM-1's) */
        if (!strcmp(a, "on") || !strcmp(a, "off"))
            plat_link_enable(!strcmp(a, "on"));
        else if (!strcmp(a, "tempo"))
            plat_link_set_tempo((int32_t)(60000000.0 / atof(b)));
        else if (!strcmp(a, "play"))
            plat_link_set_playing(atoi(b));
        return 1;
    }
    if (!strcmp(cmd, "link_peer")) {              /* link_peer BPM | link_peer tempo BPM | link_peer play 0|1 | link_peer off */
        if (!strcmp(a, "off")) {
            if (air_lk_on)
                lk_enable(&air_lk, 0, own_us());
            air_lk_on = 0;
        } else if (!strcmp(a, "tempo") && air_lk_on) {
            lk_set_tempo(&air_lk, (int32_t)(60000000.0 / atof(b)), own_us());
        } else if (!strcmp(a, "play") && air_lk_on) {
            int64_t now = own_us();
            lk_set_playing(&air_lk, atoi(b), lk_beat_at(&air_lk, now), now);
        } else if (atof(a) > 0) {
            air_peer((int32_t)(60000000.0 / atof(a)));
        }
        return 1;
    }
#endif
    return 0;
}
/* the script's expect (host/plat_host.c): 1 = known, the value in *got */
static int air_expect(const char *what, int *got)
{
    if (!strcmp(what, "wifi_state"))              /* plat_wifi_state: 5 connected */
        *got = plat_wifi_state();
    else if (!strcmp(what, "wifi_step"))          /* plat_wifi_step: 8 up */
        *got = plat_wifi_step();
    else if (!strcmp(what, "wifi_fail"))
        *got = plat_wifi_fail();
    else if (!strcmp(what, "wifi_ip_last"))       /* the address's last byte (192.168.50.N) */
        *got = (int)(plat_wifi_addr() & 255u);
    else if (!strcmp(what, "wifi_found"))         /* networks in the scan's table */
        *got = (int)wc_tab.n;
    else if (!strcmp(what, "wifi_saved"))
        *got = (int)wn_count(&wn_st);
    else if (!strcmp(what, "wifi_jam"))         /* plat_wifi_jam: 1 joined / joining an Optimist AP, 2 refused */
        *got = plat_wifi_jam();
    else if (!strcmp(what, "wifi_mode"))
        *got = plat_wifi_get_mode();
    else if (!strcmp(what, "air_dhcp_acks"))
        *got = (int)air.dhcp_acks;
    else if (!strcmp(what, "air_in"))            /* frames the access point heard from the FM-1, and sent it */
        *got = (int)air.frames_in;
    else if (!strcmp(what, "air_out"))
        *got = (int)air.frames_out;
    else if (!strcmp(what, "air_keyed"))
        *got = air.keyed;
    else if (!strncmp(what, "wifi_t_", 7)) {      /* plat_wifi_tune_info: wifi_t_on, _cal, _cal_in, _filt, _agc, _slow, _kbps, _falls, _fast */
        struct plat_wifi_tune t;
        const char *k = what + 7;
        (void)plat_wifi_tune_info(&t);
        *got = !strcmp(k, "on") ? t.on : !strcmp(k, "cal") ? t.cal : !strcmp(k, "cal_in") ? t.cal_in : !strcmp(k, "filt") ? t.filt_sta :
               !strcmp(k, "agc") ? t.agc_gain : !strcmp(k, "slow") ? t.rate_slow : !strcmp(k, "kbps") ? t.tx_kbps :
               !strcmp(k, "falls") ? (int)t.falls : !strcmp(k, "fast") ? (int)t.fast : -9999;
    }
    else if (!strcmp(what, "wifi_q_on"))          /* measuring now (someone asked in the last 15 s); asks nothing itself */
        *got = oq.on;
    else if (!strncmp(what, "wifi_q_", 7)) {      /* plat_wifi_quality: wifi_q_ok, _rssi, _kbps, _tx, _retry_pct, _lost_pct, _over, _secs */
        struct plat_wifi_quality q;
        int ok = plat_wifi_quality(&q), w = q.tx ? q.tx : 1;
        const char *k = what + 7;
        *got = !strcmp(k, "ok") ? ok : !strcmp(k, "rssi") ? q.rssi : !strcmp(k, "kbps") ? q.tx_kbps : !strcmp(k, "tx") ? q.tx :
               !strcmp(k, "retry_pct") ? q.retried * 100 / w : !strcmp(k, "lost_pct") ? q.lost * 100 / w :
               !strcmp(k, "over") ? q.rx_over : !strcmp(k, "secs") ? q.secs : -9999;
    }
#if FM1B_WIFI_AP
    else if (cli_expect(what, got))
        ;
#endif
#if FM1B_LINK
    else if (!strcmp(what, "link_peers"))
        *got = (int)plat_link_peers();
    else if (!strcmp(what, "peer_peers"))
        *got = air_lk_on ? (int)lk_peers(&air_lk) : 0;
    else if (!strcmp(what, "link_bpm_x10"))
        *got = plat_link_on() ? (int)(600000000 / plat_link_mpb()) : 0;
    else if (!strcmp(what, "peer_bpm_x10"))
        *got = air_lk_on ? (int)(600000000 / lk_mpb(&air_lk)) : 0;
    else if (!strcmp(what, "link_playing"))
        *got = plat_link_playing();
    else if (!strcmp(what, "peer_playing"))
        *got = air_lk_on && lk_playing(&air_lk);
    else if (!strcmp(what, "link_phase_us")) {    /* |the FM-1's beat - the peer's| at one time, as time at the tempo */
        int64_t now = own_us(), d = plat_link_beat_at(now) - (air_lk_on ? lk_beat_at(&air_lk, now) : 0);
        *got = (int)((d < 0 ? -d : d) * plat_link_mpb() / 1000000);
    }
#endif
    else
        return 0;
    return 1;
}
