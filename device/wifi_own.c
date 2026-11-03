/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0
 * Copyright (C) 2026 Henri David */
/* WiFi on fm1-base's own stack (fm1.toml wifi_stack = "own": FM1B_WIFI_OWN), from Optimist's Wi-Fi (its wifi/wifi_conn.c,
 * wifi_link.c, the same author): a WPA2-PSK station (wifi/wifi_sta.c), its crypto (wifi_crypto.c), a
 * small IP layer (wifi_ip.c: ARP, DHCP, IGMP, UDP), the networks kept and the scan's table (wifi_nets.c). Nothing of
 * JieLi's linked:
 * the radio through felucca-bsp's chip/fm1_wifi.h. With link = true, Ableton Link on it (link/: GPL-3.0-only).
 *
 * The radio is the Bluetooth one (ble_stack = "own": its start-up is JieLi's Wi-Fi quick start): Wi-Fi comes up on a
 * radio BT has started; with Bluetooth not started, plat_wifi_on starts it (advertising as the app has it).
 * Everything runs from the main loop (wifi_poll, once a pass), one small step a pass, never a loop that waits: the
 * scan (channel after channel, 150 ms each), the key from a password (PBKDF2, 256 of its 8192 rounds a pass), the
 * join (auth, association, the 4-way handshake), DHCP, then the IP layer and the app's UDP ports.
 *
 *   MODE OFF   nothing runs; the radio, once started, stays up.
 *   MODE MAN   the radio is up; a scan when asked (plat_wifi_scan, plat_wifi_scanning), a join when asked.
 *   MODE AUTO  as MAN, and the best saved network heard is joined, at start-up and whenever none is joined.
 *   MODE AP, AP_MAN  (wifi_ap = true: FM1B_WIFI_AP) the FM-1 is the access point (device/wifi_own_ap.c); without
 *                    wifi_ap a record's AP modes are as OFF.
 * The networks kept (four: the SSID and the WPA2 key made from the password, never the password) and the mode are a
 * record of WIFI_STORE_SIZE bytes (wifi/wifi_nets.h struct wn_store): in the app's store object OBJ_WIFI, or the app's
 * own (APP_WIFI_STORE: app_wifi_store gives it, app_wifi_store_dirty is told when it changed).
 *
 * Part of the unity build (unity.c); on the host the same file over a simulated network (host/wifi_air.c). */
#include "fm1_wifi.h"                    /* felucca-bsp's chip/ (the host: host/wifi_fake/fm1_wifi.h) */
#ifndef PLAT_HOST
#include "fm1_wifi_wlc.h"                /* the coexistence controller's set-up words for fm1_wifi_up (JieLi's: Apache-2.0) */
#endif
#include "wifi_crypto.c"
#include "wifi_sta.c"
#include "wifi_ip.c"
#include "wifi_nets.c"
#include "wifi_tune.c"                   /* the tuning's pure part (device/wifi_own_tune.c: the radio's) */
#if defined(PLAT_HOST) || FM1B_WIFI_AP
#include "wifi_ap.c"                         /* (MODE AP: wifi_own_ap.c; the host: its simulated access point too, host/wifi_air.c) */
#include "wifi_ap_dhcp.c"
#else
#include "wifi_ap_key.c"                     /* (MODE JAM: an Optimist access point's default password, no MODE AP built) */
#endif

_Static_assert(sizeof(struct plat_wifi_ap) == sizeof(struct wn_ap), "plat.h plat_wifi_ap: wifi_nets.h wn_ap");
_Static_assert(PLAT_WIFI_STORE_SIZE == WN_STORE_SIZE, "plat.h PLAT_WIFI_STORE_SIZE: wifi_nets.h WN_STORE_SIZE");
/* (int): the two lists are different enums, their values are what must match (gcc's -Wenum-compare) */
_Static_assert((int)WIFI_MODE_OFF == (int)WN_MODE_OFF && (int)WIFI_MODE_AUTO == (int)WN_MODE_AUTO &&
               (int)WIFI_MODE_MAN == (int)WN_MODE_MAN && (int)WIFI_MODE_AP == (int)WN_MODE_AP &&
               (int)WIFI_MODE_AP_MAN == (int)WN_MODE_AP_MAN && (int)WIFI_MODE_JAM == (int)WN_MODE_JAM &&
               (int)WIFI_MODE_JAM_HOST == (int)WN_MODE_JAM_HOST,
               "plat.h WIFI_MODE_*: wifi_nets.h's");

#define WC_CH_MS 150u                    /* one channel's listening in a scan */
#define WC_STEP_MS 400u                  /* auth / assoc: the request again after this */
#define WC_STEP_TRIES 6u                 /* .. up to this many sends (the uplink is weak: a long frame is lost often) */
#define WC_ATTEMPTS 3u                   /* the whole join, from AUTH, this many times before FAIL */
#define WC_HS_MS 6000u                   /* the 4-way's whole window */
#define WC_PRE_MS 150u                   /* after the deauth: the AP drops its record of us */
#define WC_LINK_MS 300u                  /* M4 sent -> DHCP: the AP needs this to take the key into use */
#define WC_RETRY_MS 20000u               /* AUTO: another round after a failure or nothing found */
#define WC_JAM_BAD_MS 15000u             /* JAM: an access point that refused the default password is left alone this long
                                          * (2 min before 2026-11-03: a JAM HOST only ever uses its default one, so a refusal
                                          * is a host still starting; the user: "both devices eventually reconnected after a while") */
#define WC_JAM_RETRY_MS 5000u            /* JAM: another round this soon while no network is joined (AUTO's is WC_RETRY_MS) */
#define WC_LOST_MS 8000u                 /* joined, and nothing heard from the AP this long (its beacons): gone */
#define WC_RADIO_MS 3000u                /* the radio is started this long after power-on at the earliest (the start's own work first) */
#define OWN_RX_MAX 1024u                 /* the largest frame taken, with its receive header (a UDP datagram of 512 B and more fits) */
#define OWN_UDP_MAX 512u                 /* the largest datagram sent */

/* ---- the radio's two buffers: main RAM below FM1_WIFI_DMA_TOP (tools/build.py checks their addresses in the map) */
static uint32_t wifi_txb[(FM1_WIFI_TX_HDR + 576u) / 4u];   /* every send is from the main loop and waits for the DMA */
static uint32_t wifi_rxb[(OWN_RX_MAX + 8u) / 4u + 1u];

/* ---- time: microseconds for the IP layer and Link (int64, no 64-bit division) */
#ifdef PLAT_HOST
static int64_t own_us(void) { return (int64_t)plat_ms() * 1000; }   /* (the simulator's own time, as its scripts run it) */
#else
static uint32_t own_t_ref, own_rem;
static int64_t own_us_ref = 1000000;
static int64_t own_us(void)                      /* from plat_ticks24 (TIMER4); called at least every 100 s */
{
    uint32_t t = plat_ticks24();
    own_rem += t - own_t_ref;
    own_t_ref = t;
    own_us_ref += own_rem / PLAT_TICKS_PER_US;
    own_rem %= PLAT_TICKS_PER_US;
    return own_us_ref;
}
#endif
static uint32_t own_ms(void) { return plat_ms(); }

/* random bytes (the SNonce, the DHCP transaction, Link's ids): the radio's random source */
static void own_rand(uint8_t *out, uint8_t n)
{
    static uint32_t s = 0x9E3779B9u;
    uint8_t i;
#if FM1B_BLE_OWN && !defined(PLAT_HOST)
    ble_hw_rand(out, n);
    return;
#endif
    for (i = 0; i < n; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        out[i] = (uint8_t)(s ^ plat_ticks24());
    }
}

/* ---- the record kept: the mode, the networks */
static struct wn_store own_store;
static struct wn_store *wn_stp = &own_store;
#define wn_st (*wn_stp)
static uint8_t own_store_ok, own_store_dirty;
static void own_store_load(void)
{
    if (own_store_ok)
        return;
    own_store_ok = 1;
#ifdef APP_WIFI_STORE
    wn_stp = (struct wn_store *)app_wifi_store();   /* the app's own record (it loaded it) */
    if (wn_st.mark != WN_MARK)
        wn_reset(&wn_st);
#else
    {
        uint8_t raw[WN_STORE_SIZE];
        if (plat_store_load(OBJ_WIFI, raw, sizeof raw) != (int)sizeof raw || !wn_load(&wn_st, raw))
            wn_reset(&wn_st);
    }
#endif
}
static void own_store_changed(void)
{
    own_store_dirty = 1;
#ifdef APP_WIFI_STORE
    app_wifi_store_dirty();
#endif
}
static void own_store_poll(void)                 /* a change saved from the main loop (a flash write) */
{
#ifndef APP_WIFI_STORE
    if (own_store_dirty) {
        uint8_t raw[WN_STORE_SIZE];
        wn_save(&wn_st, raw);
        (void)plat_store_save(OBJ_WIFI, raw, sizeof raw);
    }
#endif
    own_store_dirty = 0;
}

/* ---- the radio */
static void wt_prepare(uint32_t len, uint32_t wcid, uint32_t ack);   /* (wifi_own_tune.c: a frame at the tuning's rate) */
static void wt_radio_up(void);
static void wt_heard(int8_t rssi);
static int own_radio_started(void) { return fm1_wifi_is_up(); }
/* the Wi-Fi side up: 1 now (or already), 0 not yet (Bluetooth still starting), -1 it cannot (no radio in this build) */
static int own_radio_up(void)
{
    if (own_radio_started())
        return 1;
#if FM1B_BLE
    if (plat_ble_state() == BLE_OFF)
        plat_ble_start();                         /* the radio comes with Bluetooth's start (its advertising as the app has it) */
    if (plat_ble_state() != BLE_READY)
        return plat_ble_state() == BLE_FAILED ? -1 : 0;
#endif
#ifdef PLAT_HOST
    fm1_wifi_up(0, 0, 0u);                        /* the stand-in radio takes no set-up words */
#else
    fm1_wifi_up(fm1_wlc_hi, fm1_wlc_c, FM1_WLC_N);
#endif
    wt_radio_up();                                /* (the tuning's radio side: wifi_own_tune.c) */
    return 1;
}
static void own_tx(uint32_t len, uint32_t wcid, uint32_t ack)   /* wifi_txb's frame out */
{
    wt_prepare(len, wcid, ack);
    (void)fm1_wifi_tx(wifi_txb);
}
static uint8_t *own_frame(void) { return (uint8_t *)wifi_txb + FM1_WIFI_TX_HDR; }
static void own_mac(uint8_t mac[6])              /* the station's address: the BT address the stock firmware kept, else made
                                                  * from the chip's unique ID (the same every start), locally administered */
{
    static uint8_t m[6], have;
    if (!have) {
        uint8_t vm[10];
#ifdef PLAT_HOST
        int have_vm = 0;                           /* (the simulator: the address from its chip ID) */
#else
        int have_vm = flash_ok && st_read(0x0E9000u, vm, sizeof vm) == 0;   /* VM record 102, the SDK's VM's first */
#endif
        if (!wifi_sta_get_mac(have_vm ? vm : 0, m)) {
            const uint8_t *u = plat_chip_uid();
            unsigned i = 1;
            while (u && i < 16u && u[i] == u[0])
                i++;
            if (u && i < 16u) {
                for (i = 0; i < 6u; i++)
                    m[i] = (uint8_t)(u[i] ^ u[i + 6u] ^ (i < 4u ? u[i + 12u] : 0));
                m[0] = (uint8_t)((m[0] & 0xFCu) | 0x02u);
            }
        }
        have = 1;
    }
    memcpy(mac, m, 6);
}

/* ---- the frames received: each to f(frame, mpdu_len, rssi, arrival), up to max a pass */
static uint32_t own_rx_n, own_rx_sec, own_rx_fps;
static int32_t own_rx_rssi_sum, own_rx_rssi;
static void own_rx(void (*f)(const uint8_t *, uint32_t, int8_t, int64_t), uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max; i++) {
        int32_t n = fm1_wifi_rx(wifi_rxb, OWN_RX_MAX);
        const uint8_t *fr = (const uint8_t *)wifi_rxb + 4u;
        uint32_t mpdu;
        int64_t now;
        if (n <= 0)
            return;
        now = own_us();                           /* (the arrival: as near the DMA as the main loop gets) */
        mpdu = fm1_wifi_rx_mpdu_len(fr);
        own_rx_n++;
        own_rx_rssi_sum += (int8_t)fm1_wifi_rx_rssi(fr);
        if (mpdu + FM1_WIFI_RXWI <= (uint32_t)n)
            f(fr + FM1_WIFI_RXWI, mpdu, (int8_t)fm1_wifi_rx_rssi(fr), now);
    }
}
static void own_rx_second(void)
{
    uint32_t now = own_ms();
    if (now - own_rx_sec < 1000u)
        return;
    own_rx_sec = now;
    own_rx_fps = own_rx_n;
    own_rx_rssi = own_rx_n ? own_rx_rssi_sum / (int32_t)own_rx_n : 0;
    own_rx_n = 0;
    own_rx_rssi_sum = 0;
}

/* ---- how well the link works (plat_wifi_quality): measured only while asked for, then the MAC's counters once a second
 * into a ring of 10, and a running mean of the signal of the frames that matter (the AP's; MODE AP: each client's) */
#define OQ_SLOTS 10u
#define OQ_HOLD_MS 15000u                /* measuring stops this long after the last ask */
#define OQ_STALE_MS 3000u                /* a signal not heard this long: none */
#define OQ_RX_STA_CNT2 0x1708u           /* Ralink's: [31:16] frames lost to a full receive queue */
#define OQ_TX_STA_CNT0 0x170Cu           /* [15:0] frames never acknowledged (the retries used up) */
#define OQ_TX_STA_CNT1 0x1710u           /* [15:0] acknowledged at once, [31:16] acknowledged after a retry */
#define OQ_PEERS 5u                      /* the AP (station), or MODE AP's clients 1..4 */
static struct {
    uint8_t on, n, k;                     /* measuring; seconds in the ring; the next slot */
    uint32_t t_ask, t_sec;
    uint16_t ok[OQ_SLOTS], rty[OQ_SLOTS], fail[OQ_SLOTS], over[OQ_SLOTS];
    int16_t ema[OQ_PEERS];                /* the signal, 1/16 dBm (0: none yet) */
    uint32_t t_heard[OQ_PEERS];
} oq;
static void oq_clear_counters(void)       /* (they clear when read) */
{
    (void)fm1_wifi_mac_rd(OQ_RX_STA_CNT2);
    (void)fm1_wifi_mac_rd(OQ_TX_STA_CNT0);
    (void)fm1_wifi_mac_rd(OQ_TX_STA_CNT1);
}
static void oq_signal(uint32_t peer, int8_t rssi)   /* a frame of peer p heard (only while measuring) */
{
    int32_t e;
    if (!oq.on || peer >= OQ_PEERS || rssi >= 0)
        return;
    e = oq.ema[peer];
    e = e ? e + ((int32_t)rssi * 16 - e) / 8 : (int32_t)rssi * 16;   /* (a mean over the last ~8 frames) */
    oq.ema[peer] = (int16_t)(e < -2047 ? -2047 : e > -1 ? -1 : e);
    oq.t_heard[peer] = own_ms();
}
/* (the counters are read once a second by wifi_own_tune.c wt_cnt_poll, while measuring and while the tuning's rate watches) */
static uint8_t wt_cnt_running(void);
static void wt_cnt_read(uint32_t now);
static uint32_t wt_kbps(void);
static void oq_ask(void)                  /* measuring on (from now: the counters cleared, the ring empty) */
{
    uint32_t now = own_ms();
    oq.t_ask = now;
    if (oq.on)
        return;
    if (wt_cnt_running())
        wt_cnt_read(now);                 /* (the tuning's rate takes what was counted so far: the ring starts from now) */
    else
        oq_clear_counters();
    memset(&oq, 0, sizeof oq);
    oq.on = 1;
    oq.t_ask = oq.t_sec = now;
}
static int16_t oq_rssi(uint32_t first, uint32_t last)   /* the strongest of peers first..last heard lately, dBm (0 none) */
{
    uint32_t i, now = own_ms();
    int32_t best = 0;
    for (i = first; i <= last && i < OQ_PEERS; i++)
        if (oq.ema[i] && now - oq.t_heard[i] < OQ_STALE_MS && (!best || oq.ema[i] > best))
            best = oq.ema[i];
    return (int16_t)(best ? (best - 8) / 16 : 0);     /* (rounded: -1000 / 16ths is -62.5 -> -63) */
}
static void oq_fill(struct plat_wifi_quality *q, int ap)
{
    uint32_t i, ok = 0, r = 0, f = 0, o = 0;
    for (i = 0; i < oq.n; i++) {
        ok += oq.ok[i], r += oq.rty[i], f += oq.fail[i], o += oq.over[i];
    }
    q->rssi = ap ? oq_rssi(1, OQ_PEERS - 1u) : oq_rssi(0, 0);
    q->tx_kbps = (uint16_t)wt_kbps();     /* (the unicast data's rate now: the tuning's (f), else 1 Mb/s) */
    q->tx = (uint16_t)(ok + r + f > 65535u ? 65535u : ok + r + f);
    q->retried = (uint16_t)(r > 65535u ? 65535u : r);
    q->lost = (uint16_t)(f > 65535u ? 65535u : f);
    q->rx_over = (uint16_t)(o > 65535u ? 65535u : o);
    q->secs = oq.n;
}

/* ---- the service once joined: the IP layer and the app's UDP ports */
#define OWN_SOCKS 4u
static struct {
    uint8_t on, joined, lost;                   /* lost: the AP told us to go (deauthentication, disassociation) */
    uint32_t t_heard;                             /* ms the AP was last heard (any frame from it) */
    struct wifi_sta sta;                          /* the joined station (its keys are installed in the MAC) */
    struct wifi_ip ip;
    uint32_t n_rx, n_kept, n_tx, n_tx_err;
#if FM1B_WIFI_AP
    uint8_t ap;                                   /* MODE AP: the service is the access point's (wifi_own_ap.c) */
#endif
} wl;
static struct {
    uint16_t port;
    void (*rx)(void *ctx, uint32_t src_ip, uint16_t src_port, const uint8_t *p, uint32_t n, int64_t at_us);
    void *ctx;
} own_sock[OWN_SOCKS];
#if FM1B_LINK
static void own_link_addr(uint32_t ip);           /* link/link_own.c: the address came or went */
static void own_link_poll(int64_t now);
#endif
#if FM1B_WIFI_AP
static void wa_send(uint32_t len);                /* wifi_own_ap.c: the frame out at the right key, to one client or each */
static void wa_rx(const uint8_t *f, uint32_t len, int8_t rssi, int64_t now);
static uint32_t wa_udp_srv(struct wifi_ip *wip, const uint8_t *src_mac, const uint8_t *msg, uint32_t n, uint8_t *tx_buf);
#endif
static void wl_send(uint32_t len)                 /* what wifi_txb's frame holds, out */
{
#if FM1B_WIFI_AP
    if (wl.ap) {
        wa_send(len);
        return;
    }
#endif
    wt_prepare(len, 1u, 1u);                      /* (WCID 1, the AP: the MAC encrypts what carries the Protected bit) */
    if (fm1_wifi_tx(wifi_txb) == 0)
        wl.n_tx++;
    else
        wl.n_tx_err++;
}
static void own_udp_rx(void *ctx, int64_t now, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
                       const uint8_t *p, uint32_t n)
{
    uint32_t i;
    (void)ctx, (void)dst_ip;
    for (i = 0; i < OWN_SOCKS; i++)
        if (own_sock[i].port == dst_port && own_sock[i].rx)
            own_sock[i].rx(own_sock[i].ctx, src_ip, src_port, p, n, now);
}
static void own_on_addr(void *ctx, uint32_t ip)
{
    (void)ctx;
#if FM1B_LINK
    own_link_addr(ip);
#else
    (void)ip;
#endif
}
static void own_ip_attach(void)                   /* the app's ports and the group into a fresh IP layer */
{
    uint32_t i;
    wl.ip.send = wl_send;
    wl.ip.protect = 1;                            /* (keys are in: the MAC encrypts what carries the Protected bit) */
    wl.ip.udp_rx = own_udp_rx;
    wl.ip.on_addr = own_on_addr;
    for (i = 0; i < OWN_SOCKS; i++)
        if (own_sock[i].port)
            (void)wifi_ip_bind(&wl.ip, own_sock[i].port);
}
static uint32_t own_group;                        /* the multicast group the app joined (plat_net_join) */
static void wl_rx(const uint8_t *f, uint32_t len, int8_t rssi, int64_t now)   /* one 802.11 frame the AP sent */
{
    uint32_t tx_len = 0, hl;
    uint8_t *txf = own_frame();
    int r;
    wl.n_rx++;
    if (len >= 24u && !memcmp(f + 10, wl.sta.bssid, 6)) {   /* the AP: still there; told us to go? */
        wl.t_heard = own_ms();
        oq_signal(0, rssi);
        wt_heard(rssi);
        if ((f[0] == 0xC0u || f[0] == 0xA0u) && !memcmp(f + 4, wl.sta.mac, 6))
            wl.lost = 1;
    }
    if (len < 32u || (f[0] & 0x0Cu) != 0x08u)
        return;                                   /* (data frames only) */
    hl = (f[0] & 0x80u) ? 26u : 24u;
    if (len < hl + 8u)
        return;
    if (!wifi_ip_early_filter(&wl.ip, f, len))
        return;
    wl.n_kept++;
    if (f[hl + 6u] == 0x88u && f[hl + 7u] == 0x8Eu) {   /* EAPOL: a group rekey */
        r = wifi_sta_rx_frame(&wl.sta, f, len, txf, &tx_len);
        if (r == 1)
            wl_send(tx_len);
        return;
    }
    r = wifi_ip_rx_frame(&wl.ip, f, len, now, txf, &tx_len);
    if (r == 1)
        wl_send(tx_len);
}
static void wl_poll(void)                         /* once a main-loop pass */
{
    uint32_t tx_len;
    int64_t now;
    if (!wl.on)
        return;
#if FM1B_WIFI_AP
    if (wl.ap)
        own_rx(wa_rx, 24u);                       /* (the access point answers probes and joins among the neighbours' traffic) */
    else
#endif
    own_rx(wl_rx, 6u);                            /* (a few frames a pass: the rest wait in the MAC) */
    now = own_us();
    tx_len = wifi_ip_poll(&wl.ip, now, own_frame());
    if (tx_len)
        wl_send(tx_len);
#if FM1B_LINK
    own_link_poll(now);
#endif
}
static void wl_attach(const struct wifi_sta *s)  /* the join ended connected: keep the station for the service */
{
    wl.on = 0;
    wl.sta = *s;
    memset(wl.sta.pmk, 0, sizeof wl.sta.pmk);     /* (the PTK and GTK are all the service needs) */
    wl.joined = 1;
}
static void wl_start(void)                        /* the station is joined: DHCP, then the app's ports */
{
    int64_t now = own_us();
    wifi_ip_init(&wl.ip, wl.sta.mac, wl.sta.bssid, own_rand);
    own_ip_attach();
    wifi_ip_join(&wl.ip, own_group);
    wl.n_rx = wl.n_kept = wl.n_tx = wl.n_tx_err = 0;
    wl.lost = 0;
    wl.t_heard = own_ms();
    wl_send(wifi_ip_dhcp_start(&wl.ip, own_frame()));
    wl.ip.dhcp_t0 = now;
    wl.on = 1;
}
#if FM1B_WIFI_AP
static void wl_start_ap(const uint8_t mac[6], uint32_t ip)   /* MODE AP: the service at the access point's own address (no DHCP) */
{
    wifi_ip_ap_init(&wl.ip, mac, ip, own_rand);
    own_ip_attach();
    wl.ip.udp_srv = wa_udp_srv;                   /* (the clients' DHCP requests) */
    wifi_ip_join(&wl.ip, own_group);
    wl.n_rx = wl.n_kept = wl.n_tx = wl.n_tx_err = 0;
    wl.lost = wl.joined = 0;
    wl.ap = 1;
    wl.on = 1;
    own_on_addr(0, ip);                           /* (an address at once: Link starts) */
}
#endif
static void wl_stop(void)                         /* the service off (the station stays joined as far as the AP knows) */
{
    if (wl.on) {
        wl.on = 0;
        wl.ip.ip = 0;
        own_on_addr(0, 0);
    }
}

/* ---- the connection manager (Optimist's wifi_conn.c) */
enum { WC_OFF, WC_RADIO, WC_IDLE, WC_SCAN, WC_PMK, WC_AUTH, WC_ASSOC, WC_HS, WC_UP, WC_FAIL, WC_LINK, WC_PRE };
_Static_assert((int)WC_OFF == (int)WIFI_STEP_OFF && (int)WC_PRE == (int)WIFI_STEP_PRE &&
               (int)WC_FAIL == (int)WIFI_STEP_FAIL, "plat.h WIFI_STEP_*");
enum { WCF_NONE, WCF_NOCAL, WCF_NOAP, WCF_AUTH, WCF_ASSOC, WCF_HS, WCF_KEY, WCF_LOST };
_Static_assert((int)WCF_KEY == (int)WIFI_FAIL_KEY && (int)WCF_NOCAL == (int)WIFI_FAIL_RADIO &&
               (int)WCF_LOST == (int)WIFI_FAIL_LOST, "plat.h WIFI_FAIL_*");
static struct {
    uint8_t st, fail, ch, tries;
    uint8_t scan_want;                            /* the app asks for scans (its list is on screen, nothing joined) */
    uint8_t saved_join;                           /* the join in hand uses a saved key (no password was typed) */
    uint8_t joined_slot;                          /* 1 + the saved slot joined (0: none) */
    uint8_t attempt;                              /* the join's attempt (1..WC_ATTEMPTS) */
    uint8_t jam;                                  /* the join in hand is MODE JAM's: an Optimist access point, its default password */
    uint8_t jam_bad;                              /* the last JAM join failed (a typed password); jam_bssid / jam_t say which, when */
    uint8_t jam_bssid[6];
    uint32_t jam_t;
    uint8_t scan_once;                            /* plat_wifi_scan: one sweep asked */
    uint32_t fails;                               /* failures since the start (plat_wifi_fails) */
    uint32_t t0, t_retry, sweeps;                 /* ms of this state's start / the last failure; scan sweeps done */
    struct wn_ap ap;                              /* the AP being joined */
    struct wifi_sta sta;
    struct wc_pmk_job job;
    uint8_t pmk[32];
} wc;
/* a request by SSID: scan, the strongest AP of it, join, save on success. pass empty and a saved network of that name: its
 * key is used. One at a time; the password is wiped once the key job has it */
static struct {
    uint8_t on, sweeps_left, ssid_len;
    uint8_t ssid[WN_SSID_MAX];
    char pass[64];
} wc_req;
static struct wn_scan wc_tab;                     /* the APs heard */
#include "wifi_own_tune.c"                       /* the tuning on the radio (wifi/wifi_tune.h) */

static int wc_auto(void) { return wn_st.mode == WN_MODE_AUTO || wn_st.mode == WN_MODE_JAM; }   /* (saved networks joined by themselves) */
static int wc_mode_off(void) { return wn_st.mode == WN_MODE_OFF || wn_st.mode == WN_MODE_AP || wn_st.mode == WN_MODE_AP_MAN || wn_st.mode == WN_MODE_JAM_HOST; }
static void wc_to(uint32_t st) { wc.st = (uint8_t)st; wc.t0 = own_ms(); }
static void wc_fail(uint32_t why) { wc.fail = (uint8_t)why; wc.fails++; wc_to(WC_FAIL); wc.t_retry = own_ms(); }
static void wc_send(uint32_t len) { own_tx(len, 1u, 1u); }
static void wc_on_scan(const uint8_t *f, uint32_t len, int8_t rssi, int64_t now)
{
    struct wn_ap ap;
    (void)now;
    if (wn_parse_beacon(f, len, wc.ch, rssi, &ap))
        wn_scan_add(&wc_tab, &ap);
}
static void wc_on_join(const uint8_t *f, uint32_t len, int8_t rssi, int64_t now)
{
    uint32_t tx_len = 0;
    (void)now;
    if (len >= 24u && !memcmp(f + 10, wc.ap.bssid, 6))
        wt_heard(rssi);                           /* (the AP's signal from the handshake on: the tuning's AGC) */
    if (wc.sta.state == WIFI_STA_STATE_CONNECTED)
        return;                                   /* (joined: what follows, the AP's group key frame, is wl_poll's) */
    if (wifi_sta_rx_frame(&wc.sta, f, len, own_frame(), &tx_len) == 1 && tx_len)
        wc_send(tx_len);                          /* (M2, M4: the station built them) */
}
static void wc_drop_frame(const uint8_t *f, uint32_t len, int8_t rssi, int64_t now) { (void)f, (void)len, (void)rssi, (void)now; }

static void wc_leave(void)                        /* joined (UP, LINK): tell the AP (deauth, reason 3), the MAC's key cleared
                                                   * first so the frame goes out plain; then the service stops */
{
    if (wc.st == WC_UP || wc.st == WC_LINK) {
        wifi_sta_clear_hw_keys();
        wc_send(wifi_sta_build_deauth(&wc.sta, own_frame()));
    }
#if FM1B_WIFI_AP
    if (!wl.ap)                                   /* (the access point's service is not the station's to stop) */
#endif
    wl_stop();
    wl.joined = 0;
    wc.joined_slot = 0;
    memset(&wc_req, 0, sizeof wc_req);            /* (a request still waiting for its AP: and its password: dropped) */
}
static void wc_enable(int on)
{
    if (!on) {
        wc_leave();
        wc.scan_want = 0;
        wc_to(WC_OFF);
        return;
    }
    if (wc.st == WC_OFF || wc.st == WC_FAIL)
        wc_to(WC_RADIO);
}
static void wc_scan_clear(void) { wn_scan_clear(&wc_tab); wc.sweeps = 0; }
static void wc_join(const struct wn_ap *ap, const uint8_t *pmk, const char *pass)
{
    if (wc.st == WC_OFF || wc.st == WC_RADIO)
        return;
    wc_leave();                                   /* (one AP at a time: the old one told) */
    wifi_sta_clear_hw_keys();                     /* (the MAC as at boot: no key left from the last join) */
    wc.ap = *ap;
    wc.jam = 0;                                   /* (the caller sets it after: wc_leave above clears the last one's) */
    wc.attempt = 1;
    if (pmk) {
        memcpy(wc.pmk, pmk, 32);
        wc.saved_join = 1;
        wc_to(WC_PMK);                            /* (no rounds to do: the next pass goes on) */
        wc.job.done = 1;
    } else {
        wc.saved_join = 0;
        wc_pmk_begin(&wc.job, pass, ap->ssid, ap->ssid_len);
        wc_to(WC_PMK);
    }
}
static void wc_start_auth(void)
{
    uint8_t mac[6];
    own_mac(mac);
    wifi_sta_set_mac_regs(mac, wc.ap.bssid, wc.ap.ch);
    wifi_sta_init(&wc.sta, mac, own_rand);
    wifi_sta_set_pmk(&wc.sta, wc.pmk, wc.ap.ssid, wc.ap.ssid_len);
    wifi_sta_set_ap(&wc.sta, wc.ap.bssid);
    wc.sta.state = WIFI_STA_STATE_AUTH;
    wc.tries = 0;
    wc_to(WC_PRE);
    wc_send(wifi_sta_build_deauth(&wc.sta, own_frame()));   /* (an association left from before, a reboot: gone) */
}
static void wc_retry_or_fail(uint32_t why)        /* the join again from AUTH (a new SNonce) while attempts are left */
{
    if (wc.attempt < WC_ATTEMPTS) {
        wc.attempt++;
        wc_start_auth();
        return;
    }
    if (wc.jam) {                                 /* an Optimist access point that refuses its default password has a typed one:
                                                   * not tried again for WC_JAM_BAD_MS */
        wc.jam_bad = 1;
        memcpy(wc.jam_bssid, wc.ap.bssid, 6);
        wc.jam_t = own_ms();
    }
    wc_fail(why);
}
static void wc_sweep(void)                        /* a scan from channel 1 */
{
    wc.ch = 1;
    fm1_wifi_mac_wr(FM1_WIFI_RX_FILTER, FM1_WIFI_RX_FILTER_ALL);
    fm1_wifi_set_channel(wc.ch);
    wc_to(WC_SCAN);
}
/* a join by name; 0 accepted, -1 the manager is off (MODE OFF, AP) or the name is bad, -2 no password and not a saved
 * network, -3 the radio is not up yet */
static int wc_request(const uint8_t *ssid, uint32_t len, const char *pass)
{
    uint32_t i;
    if (wc_mode_off() || len < 1u || len > WN_SSID_MAX)
        return -1;
    if (wc.st == WC_OFF || wc.st == WC_RADIO)
        return -3;
    if ((!pass || !pass[0]) && wn_find(&wn_st, ssid, len) < 0)
        return -2;
    wc_leave();
    memset(&wc_req, 0, sizeof wc_req);
    memcpy(wc_req.ssid, ssid, len);
    wc_req.ssid_len = (uint8_t)len;
    if (pass)
        for (i = 0; pass[i] && i < 63u; i++)
            wc_req.pass[i] = pass[i];
    wc_req.on = 1;
    wc_req.sweeps_left = 3u;                      /* sweeps to find it before NOT FOUND */
    wc.joined_slot = 0;
    wc_scan_clear();
    wc_sweep();
    return 0;
}
static int wc_auto_pick(int *slot)                /* AUTO: the strongest AP of the most recent saved network heard */
{
    uint32_t i;
    for (i = 0; i < WN_SLOTS; i++) {
        int a;
        if (wn_st.n[i].used != 1u)
            continue;
        a = wn_scan_best_for(&wc_tab, wn_st.n[i].ssid, wn_st.n[i].ssid_len);
        if (a >= 0 && wc_tab.ap[a].sec == WN_SEC_PSK) {
            *slot = (int)i;
            return a;
        }
    }
    return -1;
}
/* MODE JAM: an Optimist access point is named FM1- and four capital hex digits (the automatic name of device/wifi_own_ap.c wa_name) */
static int wc_is_optimist_name(const uint8_t *s, uint32_t n)
{
    uint32_t i;
    if (n != 8u || memcmp(s, "FM1-", 4) != 0)
        return 0;
    for (i = 4; i < 8u; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F')))
            return 0;
    return 1;
}
static int wc_is_own_name(const uint8_t *s, uint32_t n)   /* never join the FM-1's own network */
{
    static const char H[] = "0123456789ABCDEF";
    uint8_t mac[6];
    own_mac(mac);
    return n == 8u && memcmp(s, "FM1-", 4) == 0 && s[4] == (uint8_t)H[mac[4] >> 4] && s[5] == (uint8_t)H[mac[4] & 15u] &&
           s[6] == (uint8_t)H[mac[5] >> 4] && s[7] == (uint8_t)H[mac[5] & 15u];
}
/* the strongest Optimist access point heard (the table is kept strongest first), WPA2-PSK, not our own, not one that refused its
 * default password in the last WC_JAM_BAD_MS: its table index, or -1 */
static int wc_jam_pick(void)
{
    uint32_t i;
    for (i = 0; i < wc_tab.n; i++) {
        const struct wn_ap *a = &wc_tab.ap[i];
        if (a->sec != WN_SEC_PSK || !wc_is_optimist_name(a->ssid, a->ssid_len) || wc_is_own_name(a->ssid, a->ssid_len))
            continue;
        if (wc.jam_bad && !memcmp(wc.jam_bssid, a->bssid, 6) && own_ms() - wc.jam_t < WC_JAM_BAD_MS)
            continue;
        return (int)i;
    }
    return -1;
}
static void wc_scan_step(void)
{
    uint32_t now = own_ms();
    int slot = 0, a;
    own_rx(wc_on_scan, 8u);
    if (now - wc.t0 < WC_CH_MS)
        return;
    if (wc.ch < 13u) {
        wc.ch++;
        fm1_wifi_set_channel(wc.ch);
        wc.t0 = now;
        return;
    }
    wc.sweeps++;
    wc.t_retry = now;
    wc.scan_once = 0;
    if (wc_req.on) {                              /* a request by name: its strongest AP, or another sweep, or NOT FOUND */
        a = wn_scan_best_for(&wc_tab, wc_req.ssid, wc_req.ssid_len);
        if (a >= 0) {
            int sv = wn_find(&wn_st, wc_req.ssid, wc_req.ssid_len);
            char pw[64];                          /* (a copy: wc_join's wc_leave wipes wc_req, and the key job takes this one) */
            wc_req.on = 0;
            if (!wc_req.pass[0] && sv < 0) {      /* (forgotten while it was being looked for) */
                memset(&wc_req, 0, sizeof wc_req);
                wc_fail(WCF_NOAP);
                return;
            }
            memcpy(pw, wc_req.pass, sizeof pw);
            memset(&wc_req, 0, sizeof wc_req);
            if (pw[0])
                wc_join(&wc_tab.ap[a], 0, pw);
            else
                wc_join(&wc_tab.ap[a], wn_st.n[sv].pmk, 0);
            memset(pw, 0, sizeof pw);
        } else if (--wc_req.sweeps_left == 0u) {
            memset(&wc_req, 0, sizeof wc_req);
            wc_fail(WCF_NOAP);
        } else {
            wc.ch = 1;
            fm1_wifi_set_channel(wc.ch);
            wc.t0 = now;
        }
        return;
    }
    if (wc_auto() && !wl.joined && (a = wc_auto_pick(&slot)) >= 0) {
        wc_join(&wc_tab.ap[a], wn_st.n[slot].pmk, 0);
        return;
    }
    if (wn_st.mode == WN_MODE_JAM && !wl.joined && (a = wc_jam_pick()) >= 0) {   /* no saved network heard: an Optimist access point */
        char k[WIFI_AP_KEY_LEN + 1u];
        struct wn_ap ap = wc_tab.ap[a];
        wifi_ap_default_key(ap.ssid, ap.ssid_len, k);
        wc_join(&ap, 0, k);
        wc.jam = 1;
        memset(k, 0, sizeof k);
        return;
    }
    if (wc.scan_want) {                           /* the app's list is on screen: another sweep, the table kept */
        wc.ch = 1;
        fm1_wifi_set_channel(wc.ch);
        wc.t0 = now;
    } else {
        wc_to(WC_IDLE);
    }
}
static void wc_poll(void)                         /* once a main-loop pass: one step */
{
    static uint8_t mode_seen = 0xFFu;
    uint32_t now = own_ms();
    if (wn_st.mode != mode_seen) {                /* MODE JAM just chosen: look at once, not after the next retry */
        mode_seen = wn_st.mode;
        if (mode_seen == WN_MODE_JAM)
            wc.t_retry = now - WC_RETRY_MS;
    }
    if (wc_mode_off() && wc.st != WC_OFF && wc.st != WC_RADIO) {   /* MODE OFF, in any state: everything stops */
        wc_enable(0);
        return;
    }
    switch (wc.st) {
    case WC_OFF:
        if (!wc_mode_off())
            wc_to(WC_RADIO);
        break;
    case WC_RADIO: {
        int r;
        if (wc_mode_off()) {
            wc_to(WC_OFF);
            break;
        }
        if (now < WC_RADIO_MS)
            break;
        r = own_radio_up();
        if (r < 0) {
            wc_fail(WCF_NOCAL);
            break;
        }
        if (r == 0)
            break;
        wc_scan_clear();
        wc_to(WC_IDLE);
        break;
    }
    case WC_IDLE:
    case WC_FAIL:
        if (wc_mode_off()) {
            wc_enable(0);
            break;
        }
        if (wc.st == WC_FAIL && wc.fail == WCF_NOCAL)
            break;
        if (wc.st == WC_FAIL && now - wc.t_retry < (wn_st.mode == WN_MODE_JAM ? WC_JAM_RETRY_MS : WC_RETRY_MS) && wc_auto() &&
            !wc.scan_want && !wc.scan_once)
            break;
        if (wc.scan_want || wc.scan_once || ((wn_st.mode == WN_MODE_JAM && now - wc.t_retry >= WC_JAM_RETRY_MS) ||
                                                   (wn_st.mode == WN_MODE_AUTO && wn_count(&wn_st) && now - wc.t_retry >= WC_RETRY_MS)))
            wc_sweep();
        break;
    case WC_SCAN:
        wc_scan_step();
        break;
    case WC_PMK:
        if (wc_pmk_step(&wc.job, 256u, wc.pmk))
            wc_start_auth();
        break;
    case WC_PRE:
        own_rx(wc_drop_frame, 24u);               /* (whatever the AP still sends from the old association) */
        if (now - wc.t0 >= WC_PRE_MS) {
            wc_to(WC_AUTH);
            wc_send(wifi_sta_build_auth(&wc.sta, own_frame()));
            wc.tries = 1;
        }
        break;
    case WC_AUTH:
        own_rx(wc_on_join, 24u);
        if (wc.sta.state == WIFI_STA_STATE_ASSOC) {
            wc_to(WC_ASSOC);
            wc_send(wifi_sta_build_assoc_req(&wc.sta, own_frame()));
            wc.tries = 1;
        } else if (now - wc.t0 >= WC_STEP_MS) {
            if (wc.tries >= WC_STEP_TRIES) {
                wc_retry_or_fail(WCF_AUTH);
            } else {
                wc.tries++;
                wc.t0 = now;
                wc_send(wifi_sta_build_auth(&wc.sta, own_frame()));
            }
        }
        break;
    case WC_ASSOC:
        own_rx(wc_on_join, 24u);
        if (wc.sta.state >= WIFI_STA_STATE_WAIT_M1) {   /* (M1, even M3, may have come in the same pass: the HS step sees) */
            wc_to(WC_HS);
        } else if (now - wc.t0 >= WC_STEP_MS) {
            if (wc.tries >= WC_STEP_TRIES) {
                wc_retry_or_fail(WCF_ASSOC);
            } else {
                wc.tries++;
                wc.t0 = now;
                wc_send(wifi_sta_build_assoc_req(&wc.sta, own_frame()));
            }
        }
        break;
    case WC_HS:
        own_rx(wc_on_join, 24u);
        if (wc.sta.state == WIFI_STA_STATE_CONNECTED) {
            wl_attach(&wc.sta);
            if (!wc.jam && wn_put(&wn_st, wc.ap.ssid, wc.ap.ssid_len, wc.sta.pmk) == 0) {   /* (a JAM join is not saved: it would take a saved network's place) */
                own_store_changed();
                wc.joined_slot = (uint8_t)(wn_find(&wn_st, wc.ap.ssid, wc.ap.ssid_len) + 1);
            }
            wc_to(WC_LINK);
        } else if (wc.sta.state == WIFI_STA_STATE_FAILED) {
            wc_retry_or_fail(WCF_KEY);            /* (a key frame failed its check: a frame corrupted on the weak link as
                                                   * often as a wrong password: retried, WRONG PASSWORD when every attempt ends so) */
        } else if (now - wc.t0 >= WC_HS_MS) {
            wc_retry_or_fail(WCF_HS);
        }
        break;
    case WC_LINK:
        own_rx(wc_drop_frame, 24u);
        if (now - wc.t0 >= WC_LINK_MS) {
            wl_start();                           /* DHCP, then the app's ports */
            wc_to(WC_UP);
        }
        break;
    case WC_UP:                                   /* (wl_poll serves the link) */
        if (wl.lost || now - wl.t_heard >= WC_LOST_MS) {   /* the AP went (out of reach, off) or sent us away */
            wifi_sta_clear_hw_keys();
            wl_stop();
            wl.joined = 0;
            wc.joined_slot = 0;
            wc_fail(WCF_LOST);
            wc.t_retry = now - WC_RETRY_MS;       /* (AUTO: looks for a network again at once) */
        }
        break;
    default:
        break;
    }
}


#if FM1B_WIFI_AP
#include "wifi_own_ap.c"                         /* MODE AP: the FM-1 as the access point */
#endif

/* ---- the main loop's turn (main_fm1.c, plat_yield) */
static uint8_t own_started;
static void wifi_log_watch(const char *s) { (void)s; }   /* (radio.c: JieLi's library's log lines; none here) */
static void wifi_poll(void)
{
    if (!own_started) {
        own_started = 1;
        own_store_load();
    }
    wc_poll();
#if FM1B_WIFI_AP
    wa_poll();
#endif
    wl_poll();
    own_rx_second();
    wt_poll();                                    /* (the tuning; the quality counters' second) */
    own_store_poll();
}

/* ---- plat.h */
void plat_wifi_on(int on)                        /* (JieLi's API: on = AUTO unless a mode is set, off = MODE OFF) */
{
    own_store_load();
    if (on && wn_st.mode == WN_MODE_OFF)
        plat_wifi_mode(WIFI_MODE_AUTO);
    else if (!on && wn_st.mode != WN_MODE_OFF)
        plat_wifi_mode(WIFI_MODE_OFF);
}
void plat_wifi_mode(int mode)
{
    own_store_load();
    if (mode < WIFI_MODE_OFF || (mode > (FM1B_WIFI_AP ? WIFI_MODE_AP_MAN : WIFI_MODE_MAN) && mode != WIFI_MODE_JAM) || wn_st.mode == (uint8_t)mode)
        return;
    wn_st.mode = (uint8_t)mode;
    own_store_changed();
}
int plat_wifi_get_mode(void) { own_store_load(); return wn_st.mode; }
int plat_wifi_step(void) { return wc.st; }
int plat_wifi_jam(void)
{
    if (wn_st.mode != WN_MODE_JAM)
        return 0;
    if (wc.jam && wc.st >= WC_PMK && wc.st != WC_FAIL)
        return 1;
    return wc.jam_bad && own_ms() - wc.jam_t < WC_JAM_BAD_MS ? 2 : 0;
}
int plat_wifi_fail(void) { return wc.fail; }
uint32_t plat_wifi_fails(void) { return wc.fails; }
int plat_wifi_state(void)
{
    switch (wc.st) {
    case WC_OFF: return WIFI_OFF;
    case WC_RADIO: return WIFI_STARTING;
    case WC_IDLE: case WC_SCAN: return WIFI_ON;
    case WC_UP: return wl.ip.ip ? WIFI_CONNECTED : WIFI_ADDRESS;
    case WC_FAIL: return WIFI_FAILED;
    default: return WIFI_JOINING;
    }
}
const char *plat_wifi_why(void)
{
    static const char *const W[] = {"", "radio did not start", "not found", "no answer", "not let in", "handshake failed",
                                    "wrong password?", "network lost"};
    if (wc.st == WC_UP && wl.ip.dhcp_state == WIFI_IP_DHCP_FAILED)
        return "no address";
    return wc.st == WC_FAIL && wc.fail < sizeof W / sizeof *W ? W[wc.fail] : "";
}
const char *plat_wifi_ssid(void)
{
    static char s[WN_SSID_MAX + 1];
    uint32_t n = wc.st >= WC_PMK && wc.st != WC_FAIL ? wc.ap.ssid_len : 0u;
    memcpy(s, wc.ap.ssid, n);
    s[n] = 0;
    return s;
}
uint32_t plat_wifi_ip(void)                      /* (first byte lowest: the IP layer's host order turned round) */
{
    uint32_t a = wl.on ? wl.ip.ip : 0u;
    return a >> 24 | (a >> 8 & 0xFF00u) | (a << 8 & 0xFF0000u) | a << 24;
}
uint32_t plat_wifi_addr(void) { return wl.on ? wl.ip.ip : 0u; }
int plat_wifi_rssi(void) { return wc.st == WC_UP ? wc.ap.rssi : 0; }
int plat_wifi_scan(void)
{
    if (wc.st < WC_IDLE || wc_mode_off())
        return -1;
    wc.scan_once = 1;
    return 0;
}
void plat_wifi_scanning(int on) { wc.scan_want = (uint8_t)(on != 0); }
int plat_wifi_found(int i, char *ssid, int *rssi, int *secured)
{
    if (i < 0 || i >= wc_tab.n)
        return 0;
    if (ssid) {
        memcpy(ssid, wc_tab.ap[i].ssid, wc_tab.ap[i].ssid_len);
        ssid[wc_tab.ap[i].ssid_len] = 0;
    }
    if (rssi)
        *rssi = wc_tab.ap[i].rssi;
    if (secured)
        *secured = wc_tab.ap[i].sec != WN_SEC_OPEN;
    return 1;
}
int plat_wifi_found_channel(int i) { return i >= 0 && i < wc_tab.n ? wc_tab.ap[i].ch : 0; }
int plat_wifi_found_ap(int i, struct plat_wifi_ap *a)
{
    if (i < 0 || i >= wc_tab.n)
        return 0;
    if (a)
        memcpy(a, &wc_tab.ap[i], sizeof *a);
    return 1;
}
int plat_wifi_joining(struct plat_wifi_ap *a)
{
    if (wc.st < WC_PMK || wc.st == WC_FAIL)
        return 0;
    if (a)
        memcpy(a, &wc.ap, sizeof *a);
    return 1;
}
int plat_wifi_joined_slot(void) { return (int)wc.joined_slot - 1; }
int plat_wifi_saved_join(void) { return wc.saved_join; }
uint32_t plat_wifi_scans(void) { return wc.sweeps; }
int plat_wifi_join_ssid(const uint8_t *ssid, uint32_t len, const char *pass) { own_store_load(); return wc_request(ssid, len, pass); }
int plat_wifi_join(const char *ssid, const char *pass)
{
    uint32_t n = 0;
    while (ssid && ssid[n])
        n++;
    return plat_wifi_join_ssid((const uint8_t *)ssid, n, pass) == 0 ? 0 : -1;
}
void plat_wifi_leave(void)
{
    wc_leave();
    if (wc.st != WC_OFF)
        wc_to(WC_IDLE);
}
int plat_wifi_saved(int i, char *ssid)
{
    own_store_load();
    if (i < 0 || i >= (int)WN_SLOTS || wn_st.n[i].used != 1u)
        return 0;
    if (ssid) {
        memcpy(ssid, wn_st.n[i].ssid, wn_st.n[i].ssid_len);
        ssid[wn_st.n[i].ssid_len] = 0;
    }
    return 1;
}
void plat_wifi_forget(int i)
{
    own_store_load();
    if (i < 0 || i >= (int)WN_SLOTS || wn_st.n[i].used != 1u)
        return;
    wn_forget(&wn_st, (uint32_t)i);
    own_store_changed();
    if (wc.joined_slot == (uint8_t)(i + 1))
        wc.joined_slot = 0;                       /* (still joined: only the key is gone) */
    else if (wc.joined_slot > (uint8_t)(i + 1))
        wc.joined_slot--;
}
int plat_wifi_kept(const char *ssid)
{
    uint32_t n = 0;
    own_store_load();
    while (ssid && ssid[n])
        n++;
    return wn_find(&wn_st, (const uint8_t *)ssid, n) >= 0;
}
void plat_wifi_rx(uint32_t *frames, int *rssi)
{
    if (frames)
        *frames = own_rx_fps;
    if (rssi)
        *rssi = own_rx_rssi;
}
int plat_net_task(void (*fn)(void *), void *arg, const char *name) { (void)fn, (void)arg, (void)name; return -1; }
int plat_wifi_quality(struct plat_wifi_quality *q)
{
    int ap = 0;
    memset(q, 0, sizeof *q);
#if FM1B_WIFI_AP
    ap = wl.ap && wl.on;
#endif
    if (!ap && wc.st != WC_UP)
        return 0;
    oq_ask();
    oq_fill(q, ap);
    return 1;
}

/* UDP for the app (main loop): its ports' datagrams to rx with their arrival (plat_wifi_us), sent from a port */
int plat_net_udp_open(uint16_t port, void (*rx)(void *ctx, uint32_t src_ip, uint16_t src_port, const uint8_t *p,
                                                uint32_t n, int64_t at_us), void *ctx)
{
    uint32_t i, k = OWN_SOCKS;
    if (!port || !rx)
        return -1;
    for (i = 0; i < OWN_SOCKS; i++) {
        if (own_sock[i].port == port)
            return -1;
        if (!own_sock[i].port && k == OWN_SOCKS)
            k = i;
    }
    if (k == OWN_SOCKS)
        return -1;
    own_sock[k].port = port;
    own_sock[k].rx = rx;
    own_sock[k].ctx = ctx;
    if (wl.on)
        (void)wifi_ip_bind(&wl.ip, port);
    return 0;
}
void plat_net_udp_close(uint16_t port)
{
    uint32_t i;
    for (i = 0; i < OWN_SOCKS; i++)
        if (own_sock[i].port == port)
            memset(&own_sock[i], 0, sizeof own_sock[i]);
    wifi_ip_unbind(&wl.ip, port);
}
int plat_net_udp_send(uint16_t src_port, uint32_t ip, uint16_t port, const void *p, uint32_t n)
{
    uint32_t len;
    if (!wl.on || !wl.ip.ip || !p || n > OWN_UDP_MAX)
        return -1;
    len = wifi_ip_send_udp(&wl.ip, src_port, ip, port, (const uint8_t *)p, n, own_frame());
    if (!len)
        return -1;
    wl_send(len);
    return 0;
}
void plat_net_join(uint32_t group)
{
    own_group = group;
    if (wl.on)
        wifi_ip_join(&wl.ip, group);
}
int64_t plat_wifi_us(void) { return own_us(); }

#if FM1B_LINK
#include "link_own.c"                             /* Ableton Link on it (link/: GPL-3.0-only, an opt-in part) */
#else
void plat_link_enable(int on) { (void)on; }
int plat_link_on(void) { return 0; }
uint32_t plat_link_peers(void) { return 0; }
int32_t plat_link_mpb(void) { return 0; }
int64_t plat_link_beat_at(int64_t us) { (void)us; return -1; }
int64_t plat_link_time_at(int64_t ub) { (void)ub; return 0; }
int64_t plat_link_phase(int64_t ub, int64_t quantum_ub) { (void)ub, (void)quantum_ub; return 0; }
int plat_link_playing(void) { return 0; }
int64_t plat_link_play_beat(void) { return 0; }
void plat_link_set_tempo(int32_t mpb) { (void)mpb; }
void plat_link_set_playing(int play) { (void)play; }
int64_t plat_link_now(void) { return 0; }
uint32_t plat_link_t24(int64_t us) { (void)us; return 0; }
#endif
