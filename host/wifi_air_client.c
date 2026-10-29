/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0
 * Copyright (C) 2026 Henri David */
/* The host simulator's client of the FM-1's own access point (MODE AP, FM1B_WIFI_AP: device/wifi_own_ap.c), as a phone
 * would be one: "wifi_client NAME PASS" (NAME "auto": the FM-1's automatic name) waits for a beacon of that name, then
 * authenticates, associates, does the 4-way handshake as the station (wifi/wifi_sta.c), asks for an address (DHCP:
 * wifi_ip.c as a station) and, with Link, is a Link peer on it ("client_peer BPM"). A deauthentication from the FM-1 (its
 * access point restarted, a key forgotten) sends it back to waiting for a beacon, as a phone joins again. All in this
 * process; nothing is encrypted (the MAC's CCMP is the hardware's). Part of host/wifi_air.c. */
#define CLI_RETRY_MS 500u
#define CLI_TRIES 8u
#define CLI_DHCP_MS 300u                          /* the handshake done -> DHCP (the FM-1 needs its key in first) */
enum { CLI_OFF, CLI_LOOK, CLI_AUTH, CLI_ASSOC, CLI_HS, CLI_KEYED, CLI_IP };
static struct {
    uint8_t st, ch, name_len, peer_want, peer_on;
    uint8_t name[WIFI_STA_SSID_MAX], mac[6], bssid[6], pmk[32];
    char pass[64];
    struct wifi_sta sta;
    struct wifi_ip ip;
    uint32_t t, tries, joins, deauths;
    int32_t peer_mpb;
    uint8_t txb[AIR_FRAME];
} cli;
#if FM1B_LINK
static struct lk_node cli_lk;
#endif

static void cli_queue(const uint8_t *f, uint32_t n)   /* a frame the client sends, to the FM-1 if it listens there */
{
    if (!n || n > AIR_FRAME || air.qw - air.qr >= AIR_Q || fm1_fake_ch != cli.ch)
        return;
    memcpy(air.q[air.qw % AIR_Q].f, f, n);
    air.q[air.qw % AIR_Q].n = n;
    air.qw++;
}
static void cli_to(uint32_t st) { cli.st = (uint8_t)st; cli.t = plat_ms(); cli.tries = 0; }
static void cli_look(void)                        /* back to waiting for the beacon (the address and Link gone) */
{
#if FM1B_LINK
    if (cli.peer_on)
        lk_enable(&cli_lk, 0, own_us());
#endif
    cli.peer_on = 0;
    memset(&cli.ip, 0, sizeof cli.ip);
    cli.ch = 0;
    cli_to(CLI_LOOK);
}
#if FM1B_LINK
static int cli_lk_send(void *ctx, uint32_t ip, uint16_t port, const uint8_t *p, uint32_t n)
{
    uint32_t len;
    (void)ctx;
    if (cli.st != CLI_IP || !cli.ip.ip)
        return -1;
    len = wifi_ip_send_udp(&cli.ip, LK_MCAST_PORT, ip, port, p, n, cli.txb);
    cli_queue(cli.txb, len);
    return len ? 0 : -1;
}
static void cli_lk_rx(void *ctx, int64_t now, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
                      const uint8_t *p, uint32_t n)
{
    (void)ctx, (void)dst_ip, (void)dst_port;
    if (cli.peer_on)
        lk_rx(&cli_lk, now, src_ip, src_port, p, n);
}
static void cli_peer_start(void)                  /* the client has an address: its Link peer on it */
{
    struct lk_cfg c;
    c.ip = cli.ip.ip;
    c.port = LK_MCAST_PORT;
    c.send = cli_lk_send;
    c.ctx = 0;
    lk_init(&cli_lk, &c, 0xC11u, cli.peer_mpb);
    lk_enable(&cli_lk, 1, own_us());
    lk_set_addr(&cli_lk, cli.ip.ip, LK_MCAST_PORT);
    cli.peer_on = 1;
}
#endif

/* a frame the FM-1 sent (air_tx), as the client hears it */
static void cli_hear(const uint8_t *f, uint32_t len)
{
    uint32_t n = 0;
    if (cli.st == CLI_OFF || len < 24u || (cli.ch && fm1_fake_ch != cli.ch))
        return;
    if (cli.st == CLI_LOOK) {                     /* its beacon: the BSSID and the channel, then authentication */
        struct wn_ap ap;
        if (!wn_parse_beacon(f, len, (uint8_t)fm1_fake_ch, -40, &ap) || ap.ssid_len != cli.name_len ||
            memcmp(ap.ssid, cli.name, cli.name_len) != 0 || f[0] != 0x80u)
            return;
        memcpy(cli.bssid, ap.bssid, 6);
        cli.ch = (uint8_t)fm1_fake_ch;
        wifi_sta_init(&cli.sta, cli.mac, own_rand);
        wifi_sta_set_pmk(&cli.sta, cli.pmk, cli.name, cli.name_len);
        wifi_sta_set_ap(&cli.sta, cli.bssid);
        cli.sta.state = WIFI_STA_STATE_AUTH;
        cli_to(CLI_AUTH);
        cli_queue(cli.txb, wifi_sta_build_auth(&cli.sta, cli.txb));
        return;
    }
    if (memcmp(f + 4, cli.mac, 6) != 0 || memcmp(f + 10, cli.bssid, 6) != 0)
        return;                                   /* (not to us: beacons, the other clients' frames) */
    if (f[0] == 0xC0u || f[0] == 0xA0u) {         /* sent away: joins again */
        cli.deauths++;
        cli_look();
        return;
    }
    if (cli.st == CLI_IP) {
        if ((f[0] & 0x0Cu) == 0x08u && wifi_ip_rx_frame(&cli.ip, f, len, own_us(), cli.txb, &n) == 1 && n)
            cli_queue(cli.txb, n);
        return;
    }
    if (cli.st < CLI_AUTH || cli.st > CLI_HS)
        return;
    if (wifi_sta_rx_frame(&cli.sta, f, len, cli.txb, &n) == 1 && n)
        cli_queue(cli.txb, n);                    /* (M2, M4) */
    if (cli.st == CLI_AUTH && cli.sta.state == WIFI_STA_STATE_ASSOC) {
        cli_to(CLI_ASSOC);
        cli_queue(cli.txb, wifi_sta_build_assoc_req(&cli.sta, cli.txb));
    } else if (cli.st == CLI_ASSOC && cli.sta.state >= WIFI_STA_STATE_WAIT_M1) {
        cli_to(CLI_HS);
    }
    if (cli.sta.state == WIFI_STA_STATE_CONNECTED) {
        cli.joins++;
        cli_to(CLI_KEYED);
    } else if (cli.sta.state == WIFI_STA_STATE_FAILED) {
        cli_look();                               /* (the wrong password: it tries again, and fails again) */
    }
}
static void cli_tick(void)                        /* every simulated millisecond */
{
    uint32_t now = plat_ms(), n;
    if (cli.st == CLI_AUTH || cli.st == CLI_ASSOC) {   /* no answer: again, then back to the beacon */
        if (now - cli.t < CLI_RETRY_MS)
            return;
        if (++cli.tries >= CLI_TRIES) {
            cli_look();
            return;
        }
        cli.t = now;
        cli_queue(cli.txb, cli.st == CLI_AUTH ? wifi_sta_build_auth(&cli.sta, cli.txb) : wifi_sta_build_assoc_req(&cli.sta, cli.txb));
    } else if (cli.st == CLI_KEYED && now - cli.t >= CLI_DHCP_MS) {
        wifi_ip_init(&cli.ip, cli.mac, cli.bssid, own_rand);
        cli.ip.protect = 1;
#if FM1B_LINK
        cli.ip.udp_rx = cli_lk_rx;
        (void)wifi_ip_bind(&cli.ip, LK_MCAST_PORT);
        wifi_ip_join(&cli.ip, LK_MCAST_IP);
#endif
        cli_to(CLI_IP);
        cli_queue(cli.txb, wifi_ip_dhcp_start(&cli.ip, cli.txb));
        cli.ip.dhcp_t0 = own_us();
    } else if (cli.st == CLI_IP) {
        n = wifi_ip_poll(&cli.ip, own_us(), cli.txb);
        if (n)
            cli_queue(cli.txb, n);
#if FM1B_LINK
        if (cli.peer_want && !cli.peer_on && cli.ip.ip)
            cli_peer_start();
        if (cli.peer_on)
            lk_poll(&cli_lk, own_us());
#endif
    }
}
static void cli_start(const char *name, const char *pass)   /* the client on (name 0: off, the FM-1 told) */
{
    static const uint8_t MAC[6] = {0x02, 0xC1, 0x1E, 0x00, 0x00, 0x02};
    uint32_t n = 0;
    if (cli.st >= CLI_AUTH && cli.ch == fm1_fake_ch) {   /* leaving: a deauthentication, as a phone switching Wi-Fi off */
        cli_queue(cli.txb, wifi_sta_build_deauth(&cli.sta, cli.txb));
    }
    cli_look();
    cli.st = CLI_OFF;
    if (!name)
        return;
    if (!strcmp(name, "auto")) {
        char b[WIFI_STA_SSID_MAX + 1];
        n = (uint32_t)plat_wifi_ap_name(b);
        memcpy(cli.name, b, n);
    } else {
        while (name[n] && n < WIFI_STA_SSID_MAX)
            n++;
        memcpy(cli.name, name, n);
    }
    cli.name_len = (uint8_t)n;
    memcpy(cli.mac, MAC, 6);
    wc_pmk(pass, cli.name, n, cli.pmk);
    cli_to(CLI_LOOK);
}

/* the script's "wifi ..." of MODE AP (air_cmd): ap NAME|auto PASS, ap_channel N, ap_run 0|1, ap_forget, ap_stop */
static void cli_wifi_cmd(const char *a, const char *b, const char *c)
{
    if (!strcmp(a, "ap")) {
        uint32_t n = !strcmp(b, "auto") ? 0u : (uint32_t)strlen(b);
        printf("  wifi ap %s: %d\n", b, plat_wifi_ap_set((const uint8_t *)b, n, c));
    } else if (!strcmp(a, "ap_channel")) {
        printf("  wifi ap_channel %s: %d\n", b, plat_wifi_ap_set_channel(atoi(b)));
    } else if (!strcmp(a, "ap_run")) {
        printf("  wifi ap_run %s: %d\n", b, plat_wifi_ap_run(atoi(b)));
    } else if (!strcmp(a, "ap_forget")) {
        plat_wifi_ap_forget();
    } else if (!strcmp(a, "ap_stop")) {
        plat_wifi_ap_stop();
    }
}
/* wifi_client NAME|auto PASS | wifi_client off; client_peer BPM | client_peer play 0|1 | client_peer off;
 * wifi_ap_start NAME PASS CH (one not kept, MODE OFF) */
static int cli_cmd(const char *cmd, const char *a, const char *b, const char *c)
{
    if (!strcmp(cmd, "wifi_client")) {
        cli_start(strcmp(a, "off") ? a : 0, b);
        return 1;
    }
    if (!strcmp(cmd, "wifi_ap_start")) {
        printf("  wifi_ap_start %s: %d\n", a, plat_wifi_ap_start((const uint8_t *)a, (uint32_t)strlen(a), b, atoi(c)));
        return 1;
    }
#if FM1B_LINK
    if (!strcmp(cmd, "client_peer")) {
        if (!strcmp(a, "off")) {
            if (cli.peer_on)
                lk_enable(&cli_lk, 0, own_us());
            cli.peer_on = cli.peer_want = 0;
        } else if (!strcmp(a, "play") && cli.peer_on) {
            int64_t now = own_us();
            lk_set_playing(&cli_lk, atoi(b), lk_beat_at(&cli_lk, now), now);
        } else if (atof(a) > 0) {
            cli.peer_mpb = (int32_t)(60000000.0 / atof(a));
            cli.peer_want = 1;
        }
        return 1;
    }
#endif
    return 0;
}
/* the script's expects (air_expect): 1 = known */
static int cli_expect(const char *what, int *got)
{
    uint8_t mac[6];
    uint32_t ip = 0;
    if (!strcmp(what, "ap_state"))                /* plat_wifi_ap_state: 3 up */
        *got = plat_wifi_ap_state();
    else if (!strcmp(what, "ap_clients"))
        *got = (int)plat_wifi_ap_clients();
    else if (!strcmp(what, "ap_channel"))
        *got = plat_wifi_ap_channel();
    else if (!strcmp(what, "ap_key"))
        *got = plat_wifi_ap_has_key();
    else if (!strcmp(what, "ap_client_ip_last"))  /* the first client's address, as the FM-1 gave it (192.168.4.N) */
        *got = plat_wifi_ap_client(0, mac, &ip) ? (int)(ip & 255u) : 0;
    else if (!strcmp(what, "ap_stray"))
        *got = (int)wa.n.stray;
    else if (!strcmp(what, "client_state"))       /* 5 keyed, 6 with an address asked for / got */
        *got = cli.st;
    else if (!strcmp(what, "client_joins"))
        *got = (int)cli.joins;
    else if (!strcmp(what, "client_deauths"))
        *got = (int)cli.deauths;
    else if (!strcmp(what, "client_ip_last"))     /* the client's address (192.168.4.N) */
        *got = cli.st == CLI_IP && cli.ip.dhcp_state == WIFI_IP_DHCP_BOUND ? (int)(cli.ip.ip & 255u) : 0;
#if FM1B_LINK
    else if (!strcmp(what, "client_peers"))
        *got = cli.peer_on ? (int)lk_peers(&cli_lk) : 0;
    else if (!strcmp(what, "client_bpm_x10"))
        *got = cli.peer_on ? (int)(600000000 / lk_mpb(&cli_lk)) : 0;
    else if (!strcmp(what, "client_playing"))
        *got = cli.peer_on && lk_playing(&cli_lk);
    else if (!strcmp(what, "client_phase_us")) {  /* |the FM-1's beat - the client peer's| at one time, as time */
        int64_t now = own_us(), d = plat_link_beat_at(now) - (cli.peer_on ? lk_beat_at(&cli_lk, now) : 0);
        *got = (int)((d < 0 ? -d : d) * plat_link_mpb() / 1000000);
    }
#endif
    else
        return 0;
    return 1;
}
