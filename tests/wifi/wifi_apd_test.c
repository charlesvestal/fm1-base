/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David; in fm1-base: tests/wifi/run.sh */
/* MODE AP (device/wifi_own_ap.c, from Optimist's tests/wifi_apd_test.c) on the host: device/wifi_own.c as the unity
 * has it (wifi_ap = true, the radio host/wifi_fake/fm1_wifi.h): the frames it sends are logged, the MAC registers it
 * writes recorded, and the stack's own station (wifi_sta.c) and IP layer (wifi_ip.c) are the clients. Covers the start
 * from the key kept, a new password's key job, the beacons, a probe, a client's join (authentication, association, the
 * 4-way handshake, its keys at its own WCID), DHCP, ARP, ping and a group datagram through the keys, a stranger told to
 * join again, a second client, group frames sent to each, a client leaving, one starting over, a wrong password (M1
 * repeated, then let go), a channel change, MODE leaving AP, the key forgotten, the automatic name, the radio not
 * starting, AP MAN, and one not kept (plat_wifi_ap_start). Exit status: the number of failed checks. */
#include <stdio.h>
#include <string.h>
#include "plat.h"

/* ---- the platform, as wifi_own.c uses it */
static uint32_t ms = 5000u;
uint32_t plat_ms(void) { return ms; }
uint32_t plat_ticks24(void) { return ms * 24000u; }
const uint8_t *plat_chip_uid(void) { return 0; }
static int radio_ok = 1, radio_starts, saves;
int plat_ble_state(void) { return radio_ok ? BLE_READY : BLE_FAILED; }
void plat_ble_start(void) { radio_starts++; }
enum { OBJ_WIFI = 2 };
int plat_store_load(uint32_t obj, void *dst, uint32_t max) { (void)obj, (void)dst, (void)max; return -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { (void)obj, (void)src, (void)len; saves++; return 0; }
#include "../../device/wifi_own.c"

static int fails, checks;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}

/* ---- the stand-in radio: every frame sent is logged, every register write recorded */
#define TXMAX 96
static struct txe { uint8_t f[600]; uint32_t len, wcid, ack; } txlog[TXMAX];
static uint32_t ntx;
static int log_tx(const uint8_t *frame, uint32_t len, uint32_t wcid, uint32_t ack)
{
    struct txe *e = &txlog[ntx % TXMAX];
    memcpy(e->f, frame, len);
    e->len = len, e->wcid = wcid, e->ack = ack;
    ntx++;
    return 0;
}
static uint32_t regs[0x8000 / 4];
static int in_client;                             /* the clients' own station code writes MAC registers too (its keys): not ours */
static void reg_wr(uint32_t r, uint32_t v) { if (!in_client && r < sizeof regs) regs[r / 4] = v; }
static uint32_t reg(uint32_t r) { return regs[r / 4]; }

/* ---- helpers */
static const char *PW = "password1";
static const uint8_t NAME[7] = {'F', 'M', '1', 'T', 'E', 'S', 'T'};
static uint8_t AP_MAC[6];
static int64_t now_us(void) { return own_us(); }
static void tick(uint32_t n) { uint32_t i; for (i = 0; i < n; i++) { ms++; if (i % 5u == 0u) wifi_poll(); } wifi_poll(); }
static void run_until_up(void)
{
    uint32_t i;
    for (i = 0; i < 400u && wa.st != WA_UP; i++) {
        ms += 10u;
        wifi_poll();
    }
}
static uint32_t count_sub(uint32_t from, uint8_t fc0)
{
    uint32_t i, n = 0;
    for (i = from; i < ntx; i++)
        n += txlog[i % TXMAX].f[0] == fc0;
    return n;
}
static const struct txe *last_sub(uint8_t fc0)
{
    uint32_t i;
    for (i = ntx; i > 0 && i + TXMAX > ntx; i--)
        if (txlog[(i - 1u) % TXMAX].f[0] == fc0)
            return &txlog[(i - 1u) % TXMAX];
    return 0;
}
static void rx(const uint8_t *f, uint32_t n) { uint8_t cp[700]; memcpy(cp, f, n); wa_rx(cp, n, -40, now_us()); }

struct client {
    struct wifi_sta sta;
    uint8_t mac[6];
    uint32_t cur;                                 /* the next txlog entry to read */
};
static void client_init(struct client *c, uint8_t last, const uint8_t pmk[32], const uint8_t *ssid, uint32_t sn)
{
    memset(c, 0, sizeof *c);
    c->mac[0] = 0x02, c->mac[1] = 0x11, c->mac[2] = 0x22, c->mac[3] = 0x33, c->mac[4] = 0x44, c->mac[5] = last;
    wifi_sta_init(&c->sta, c->mac, own_rand);
    wifi_sta_set_pmk(&c->sta, pmk, ssid, sn);
    wifi_sta_set_ap(&c->sta, AP_MAC);
    c->sta.state = WIFI_STA_STATE_AUTH;
    c->cur = ntx;
}
static void pump(struct client *c)                /* what the AP sent this client since the last call, and its answers back */
{
    uint8_t out[600];
    uint32_t n;
    int r;
    while (c->cur < ntx) {
        const struct txe *e = &txlog[c->cur % TXMAX];
        c->cur++;
        if (memcmp(e->f + 4, c->mac, 6) != 0 || e->f[0] == 0xC0)
            continue;
        in_client = 1;
        n = 0;
        r = wifi_sta_rx_frame(&c->sta, e->f, e->len, out, &n);
        in_client = 0;
        if (r == 1 && n)
            rx(out, n);
    }
}
static void client_join(struct client *c)
{
    uint8_t f[200];
    rx(f, wifi_sta_build_auth(&c->sta, f));
    pump(c);
    rx(f, wifi_sta_build_assoc_req(&c->sta, f));
    pump(c);
    tick(WA_M1_DELAY_MS + 5u);                    /* M1 */
    pump(c);                                      /* M2 back, M3 out */
    pump(c);                                      /* M4 back */
}
static uint32_t cl_frame(uint8_t *f, const uint8_t mac[6], uint16_t type, uint8_t qos)   /* a client's data frame (ToDS),
                                                                                         * as the MAC hands it over decrypted */
{
    uint32_t hl = qos ? 26u : 24u;
    memset(f, 0, 64);
    f[0] = qos ? 0x88 : 0x08;
    f[1] = 0x41;
    memcpy(f + 4, AP_MAC, 6);
    memcpy(f + 10, mac, 6);
    memset(f + 16, 0xFF, 6);
    f[hl] = 0xAA, f[hl + 1] = 0xAA, f[hl + 2] = 0x03;
    f[hl + 6] = (uint8_t)(type >> 8), f[hl + 7] = (uint8_t)type;
    return hl + 8u;
}
static uint16_t csum(const uint8_t *p, uint32_t n)
{
    uint32_t s = 0, i;
    for (i = 0; i + 1u < n; i += 2u)
        s += (uint32_t)p[i] << 8 | p[i + 1u];
    if (n & 1u)
        s += (uint32_t)p[n - 1u] << 8;
    while (s >> 16)
        s = (s & 0xFFFFu) + (s >> 16);
    return (uint16_t)~s;
}
static uint32_t group_rx;
static void on_group(void *ctx, uint32_t src_ip, uint16_t src_port, const uint8_t *p, uint32_t n, int64_t at)
{
    (void)ctx, (void)src_port, (void)at;
    if (src_ip == 0xC0A80402u && n == 4u && !memcmp(p, "LINK", 4))
        group_rx++;
}

static void dhcp_ping(const struct client *A)     /* section 6: DHCP, ARP, ping and a group datagram through A's keys */
{
    struct wifi_ip cip;
    uint8_t f[700], o[700];
    uint32_t n, on = 0, mark, off, k;
    wifi_ip_init(&cip, A->mac, AP_MAC, own_rand);
    cip.protect = 1;
    mark = ntx;
    rx(f, wifi_ip_dhcp_start(&cip, f));
    check(ntx == mark + 1u && txlog[mark % TXMAX].wcid == 1u && txlog[mark % TXMAX].ack == 1u && (txlog[mark % TXMAX].f[1] & 0x40),
          "DHCP Discover: an Offer back, protected, at WCID 1, unicast");
    n = txlog[mark % TXMAX].len;
    memcpy(o, txlog[mark % TXMAX].f, n);
    check(wifi_ip_rx_frame(&cip, o, n, now_us(), f, &on) == 1 && on > 0u, "the client takes the Offer and asks (Request)");
    mark = ntx;
    rx(f, on);
    n = txlog[mark % TXMAX].len;
    memcpy(o, txlog[mark % TXMAX].f, n);
    (void)wifi_ip_rx_frame(&cip, o, n, now_us(), f, &on);
    check(cip.dhcp_state == WIFI_IP_DHCP_BOUND && cip.ip == 0xC0A80402u && cip.gw == 0xC0A80401u, "the Ack: bound to 192.168.4.2, the AP as the router");
    check(wa.n.dhcp == 2u, "the server counted two replies");
    {
        uint32_t ip = 0;
        uint8_t m[6];
        check(plat_wifi_ap_client(0, m, &ip) == 1 && ip == 0xC0A80402u && !memcmp(m, A->mac, 6), "plat_wifi_ap_client: A, 192.168.4.2");
    }
    off = cl_frame(f, A->mac, 0x0806, 1);         /* ARP request for the AP, in a QoS frame */
    f[off + 1] = 1, f[off + 2] = 8, f[off + 4] = 6, f[off + 5] = 4, f[off + 7] = 1;
    memcpy(f + off + 8, A->mac, 6);
    f[off + 14] = 192, f[off + 15] = 168, f[off + 16] = 4, f[off + 17] = 2;
    f[off + 24] = 192, f[off + 25] = 168, f[off + 26] = 4, f[off + 27] = 1;
    mark = ntx;
    rx(f, off + 28u);
    check(ntx == mark + 1u && txlog[mark % TXMAX].wcid == 1u && txlog[mark % TXMAX].f[32 + 7] == 2, "a QoS ARP request for the AP: a reply at WCID 1");
    off = cl_frame(f, A->mac, 0x0800, 0);         /* ping */
    {
        uint8_t *ip = f + off, *ic = f + off + 20;
        uint16_t c;
        ip[0] = 0x45, ip[2] = 0, ip[3] = 20 + 8 + 16, ip[8] = 64, ip[9] = 1;
        ip[12] = 192, ip[13] = 168, ip[14] = 4, ip[15] = 2, ip[16] = 192, ip[17] = 168, ip[18] = 4, ip[19] = 1;
        ic[0] = 8;
        for (k = 0; k < 16u; k++)
            ic[8 + k] = (uint8_t)k;
        ic[2] = ic[3] = 0;
        c = csum(ic, 24);
        ic[2] = (uint8_t)(c >> 8), ic[3] = (uint8_t)c;
    }
    mark = ntx;
    rx(f, off + 20u + 24u);
    check(ntx == mark + 1u && txlog[mark % TXMAX].wcid == 1u && txlog[mark % TXMAX].f[32 + 20] == 0 && csum(txlog[mark % TXMAX].f + 32 + 20, 24) == 0,
          "a ping from A: an echo reply at WCID 1 with a good checksum");
    check(wa.n.data > 0u && wl.n_kept > 0u, "the counters saw data from A");
    check(plat_net_udp_open(20808, on_group, 0) == 0, "the app's port (Link's) opened on the access point");
    plat_net_join(0xE04C4E4Bu);
    off = cl_frame(f, A->mac, 0x0800, 0);         /* a datagram to the group, from A */
    memset(f + off, 0, 40);
    f[off] = 0x45, f[off + 3] = 20 + 8 + 4, f[off + 8] = 1, f[off + 9] = 17;
    f[off + 12] = 192, f[off + 13] = 168, f[off + 14] = 4, f[off + 15] = 2;
    f[off + 16] = 224, f[off + 17] = 76, f[off + 18] = 78, f[off + 19] = 75;
    f[off + 20] = 20808 >> 8, f[off + 21] = 20808 & 255, f[off + 22] = 20808 >> 8, f[off + 23] = 20808 & 255, f[off + 25] = 8 + 4;
    memcpy(f + off + 28, "LINK", 4);
    rx(f, off + 20u + 12u);
    check(group_rx == 1u, "a datagram to the group from A reaches the app's port");
    /* a station we do not know sends data: told to join again (reason 7), once a second at most */
    off = cl_frame(f, (const uint8_t *)"\x02\x11\x22\x33\x44\xEE", 0x0800, 0);
    f[off] = 0x45, f[off + 3] = 28, f[off + 9] = 1, f[off + 19] = 1, f[off + 16] = 192, f[off + 17] = 168, f[off + 18] = 4;
    f[off + 20] = 8;
    mark = ntx;
    ms += 2000u;
    rx(f, off + 28u);
    check(ntx == mark + 1u && txlog[mark % TXMAX].f[0] == 0xC0 && txlog[mark % TXMAX].f[24] == 7 && txlog[mark % TXMAX].wcid == WA_PLAIN &&
          memcmp(txlog[mark % TXMAX].f + 4, "\x02\x11\x22\x33\x44\xEE", 6) == 0, "data from a station not ours: deauthentication, reason 7, plain");
    rx(f, off + 28u);
    check(ntx == mark + 1u && wa.n.stray == 1u, "... once a second at most");
    ms += 1100u;
    rx(f, off + 28u);
    check(ntx == mark + 2u && wa.n.stray == 2u, "... and again after a second");
}

int main(void)
{
    uint8_t pmk[32], pmk_other[32];
    struct client A, B, C;
    uint32_t i, mark;
    const struct txe *e;

    wifi_sta_set_test_mac_wr(reg_wr);
    fm1_fake_tx = log_tx;
    wc_pmk(PW, NAME, sizeof NAME, pmk);
    wc_pmk("not-the-one", NAME, sizeof NAME, pmk_other);

    /* 1. MODE AP with no key: nothing runs */
    plat_wifi_mode(WIFI_MODE_AP);
    wifi_poll();
    tick(100);
    check(wa.st == WA_OFF && plat_wifi_ap_state() == WIFI_AP_NOKEY && ntx == 0 && !fm1_fake_up, "no key: nothing started, state NOKEY");
    check(plat_wifi_step() == WIFI_STEP_OFF, "the station's manager rests in MODE AP");
    plat_wifi_mode(WIFI_MODE_OFF);
    check(plat_wifi_ap_state() == WIFI_AP_OFF, "MODE not AP: state OFF");
    plat_wifi_mode(WIFI_MODE_AP);

    /* 2. a password: the key made a slice a pass and kept, the AP up on the default channel */
    check(plat_wifi_ap_set(NAME, sizeof NAME, "short") == -1 && plat_wifi_ap_set(NAME, 33, PW) == -1 &&
          plat_wifi_ap_set(NAME, sizeof NAME, "bad\tpassword") == -1, "a short password, a 33-octet name, a control character: refused");
    check(plat_wifi_ap_set(NAME, sizeof NAME, PW) == 0 && wa.st == WA_PMK && plat_wifi_ap_state() == WIFI_AP_STARTING, "a password: the key job starts");
    wifi_poll();
    check(wa.st == WA_PMK && wa.pass[0] == 'p', "the job runs a slice a pass, not at once");
    saves = 0;
    run_until_up();
    check(wa.st == WA_UP && plat_wifi_ap_state() == WIFI_AP_UP, "the AP is up after the key job and the radio");
    check(wn_st.ap.set == 1u && memcmp(wn_st.ap.pmk, pmk, 32) == 0 && wn_st.ap.ssid_len == sizeof NAME && plat_wifi_ap_has_key(),
          "the key kept is the PBKDF2 of the password and the name");
    check(saves >= 1, "the record was saved");
    for (i = 0; i < sizeof wa.pass && wa.pass[i] == 0; i++)
        ;
    check(i == sizeof wa.pass, "the password is wiped once the key job has it");
    check(wa.ch == 6u && plat_wifi_ap_channel() == 6 && wa.ssid_len == sizeof NAME, "the default channel 6, the chosen name");
    {
        char nm[33];
        check(plat_wifi_ap_name(nm) == 7 && !strcmp(nm, "FM1TEST"), "plat_wifi_ap_name");
    }
    memcpy(AP_MAC, wa.ap.mac, 6);
    check(reg(0x1008) == ((uint32_t)AP_MAC[0] | (uint32_t)AP_MAC[1] << 8 | (uint32_t)AP_MAC[2] << 16 | (uint32_t)AP_MAC[3] << 24),
          "the radio's address is in the MAC");
    check(reg(0x1010) == reg(0x1008), "the BSSID is our address");
    check(wl.on == 1 && wl.ap == 1 && wl.ip.ip == 0xC0A80401u && wl.ip.ap == 1u && plat_wifi_addr() == 0xC0A80401u,
          "the service runs as the AP at 192.168.4.1");

    /* 3. beacons every 102 ms, plain, no ACK wanted */
    mark = ntx;
    tick(500);
    check(count_sub(mark, 0x80) >= 4u && count_sub(mark, 0x80) <= 6u, "about one beacon every 102 ms");
    e = last_sub(0x80);
    check(e && e->wcid == WA_PLAIN && e->ack == 0u, "a beacon: WCID 8 (never keyed), no ACK");
    check(e && e->f[37] == sizeof NAME && memcmp(e->f + 38, NAME, sizeof NAME) == 0, "the beacon carries the name");
    check(e && e->f[38 + 7 + 10 + 2] == 6, "the beacon's DS parameter: channel 6");

    /* 4. a probe request is answered, to the asker */
    {
        uint8_t pr[40];
        memset(pr, 0, sizeof pr);
        pr[0] = 0x40;
        memset(pr + 4, 0xFF, 6);
        memcpy(pr + 10, "\x02\x11\x22\x33\x44\xAA", 6);
        memset(pr + 16, 0xFF, 6);
        mark = ntx;
        rx(pr, 26);
        e = last_sub(0x50);
        check(ntx == mark + 1u && e && e->wcid == WA_PLAIN && e->ack == 1u && memcmp(e->f + 4, pr + 10, 6) == 0, "a wildcard probe: one answer, plain, to the asker, with ACK");
    }

    /* 5. client A joins: the handshake, its keys at WCID 1 */
    client_init(&A, 0xA1, pmk, NAME, sizeof NAME);
    client_join(&A);
    check(A.sta.state == WIFI_STA_STATE_CONNECTED, "A: the station reaches CONNECTED over the AP's frames");
    check(wa.keyed[0] == 1 && plat_wifi_ap_clients() == 1u && wa.n.m2 == 1u && wa.n.m4 == 1u && wa.n.assoc == 1u, "A: M2 and M4 accepted, keyed");
    check(memcmp(A.sta.gtk, wa.ap.gtk, 16) == 0, "A: the group key arrived");
    check(reg(0x6804) == 9u && reg(0x6008) == 0x20000001u && reg(0x600C) == 0u, "A: WCID 1 attribute CCMP pairwise, IV 0x20000001");
    check(reg(0x1808) == ((uint32_t)A.mac[0] | (uint32_t)A.mac[1] << 8 | (uint32_t)A.mac[2] << 16 | (uint32_t)A.mac[3] << 24) &&
          (reg(0x180C) & 0xFFFFu) == ((uint32_t)A.mac[4] | (uint32_t)A.mac[5] << 8), "A: its address is WCID 1's");
    {
        uint32_t k, same = 1;
        for (k = 0; k < 4u; k++)
            same &= reg(0x4020 + 4 * k) == ((uint32_t)A.sta.ptk.tk[4 * k] | (uint32_t)A.sta.ptk.tk[4 * k + 1] << 8 |
                                           (uint32_t)A.sta.ptk.tk[4 * k + 2] << 16 | (uint32_t)A.sta.ptk.tk[4 * k + 3] << 24);
        check(same, "A: the pairwise key in the MAC is the station's TK");
    }
    check(reg(0x6C00 + 32 * 1) != 0u || reg(0x6C00 + 32 * 1 + 4) != 0u, "the group key is in the shared key table");
    check(reg(0x7000) == (4u << 4), "the shared key mode: CCMP for key id 1");
    check(wa.t_due[0] == 0u, "A: no handshake send left due");

    /* 6. A: DHCP, ARP, ping, a group datagram; a stranger */
    dhcp_ping(&A);

    /* 7. client B at WCID 2; group frames go to each client at its own WCID */
    client_init(&B, 0xB2, pmk, NAME, sizeof NAME);
    client_join(&B);
    check(B.sta.state == WIFI_STA_STATE_CONNECTED && wa.keyed[1] == 1 && plat_wifi_ap_clients() == 2u, "B: joined at the second slot");
    check(reg(0x6808) == 9u && reg(0x6010) == 0x20000001u && reg(0x1810) != 0u, "B: WCID 2 attribute, IV, address");
    check(reg(0x4040) != reg(0x4020) || reg(0x4044) != reg(0x4024), "B: its own pairwise key");
    mark = ntx;
    check(plat_net_udp_send(20808, 0xE04C4E4Bu, 20808, "HELLO", 5) == 0, "a datagram to the group sent");
    check(ntx == mark + 2u, "a group frame: one frame for each client");
    check(txlog[mark % TXMAX].wcid == 1u && memcmp(txlog[mark % TXMAX].f + 4, A.mac, 6) == 0 && txlog[(mark + 1u) % TXMAX].wcid == 2u &&
          memcmp(txlog[(mark + 1u) % TXMAX].f + 4, B.mac, 6) == 0, "the copies: A at WCID 1, B at WCID 2, each to its own address");
    check(txlog[mark % TXMAX].f[32 + 16] == 224 && txlog[mark % TXMAX].f[32 + 17] == 76 && (txlog[mark % TXMAX].f[1] & 0x40),
          "the group address stays in the IP header; Protected");
    check(plat_net_udp_send(20808, 0xC0A80409u, 1, "x", 1) == -1, "an address nobody holds is not sent to");

    /* 8. A leaves: its slot and keys go; B stays */
    {
        uint8_t dea[30];
        memset(dea, 0, sizeof dea);
        dea[0] = 0xC0;
        memcpy(dea + 4, AP_MAC, 6), memcpy(dea + 10, A.mac, 6), memcpy(dea + 16, AP_MAC, 6);
        rx(dea, 26);
        check(wa.keyed[0] == 0 && plat_wifi_ap_clients() == 1u && reg(0x6804) == 0u && reg(0x6008) == 0u && reg(0x4020) == 0u, "A leaves: slot free, WCID 1 cleared");
        check(wa.keyed[1] == 1 && reg(0x6808) == 9u && plat_wifi_ap_client(0, 0, 0) == 0, "B is untouched");
    }

    /* 9. B starts over (authentication while keyed): its keys dropped, the join from the start */
    {
        uint8_t f[200];
        B.sta.state = WIFI_STA_STATE_AUTH;
        rx(f, wifi_sta_build_auth(&B.sta, f));
        check(wa.keyed[1] == 0 && reg(0x6808) == 0u, "B authenticates again: its old keys are dropped");
        B.cur = ntx - 1u;
        pump(&B);
        rx(f, wifi_sta_build_assoc_req(&B.sta, f));
        pump(&B);
        tick(WA_M1_DELAY_MS + 5u);
        pump(&B);
        pump(&B);
        check(B.sta.state == WIFI_STA_STATE_CONNECTED && wa.keyed[1] == 1, "B joins again with fresh keys");
    }

    /* 10. the wrong password: M2 fails its check, M1 repeated, then the client let go (reason 15) */
    {
        uint8_t f[200];
        uint32_t m1s = 0;
        client_init(&C, 0xC3, pmk_other, NAME, sizeof NAME);
        mark = ntx;
        rx(f, wifi_sta_build_auth(&C.sta, f));
        pump(&C);
        rx(f, wifi_sta_build_assoc_req(&C.sta, f));
        pump(&C);
        tick(WA_M1_DELAY_MS + 5u);
        pump(&C);
        check(wa.n.bad >= 1u && wa.keyed[wa_slot_of(C.mac)] == 0, "wrong password: M2 refused, not keyed");
        for (i = 0; i < 5u; i++) {
            tick(WA_HS_RETRY_MS + 20u);
            pump(&C);
        }
        for (i = mark; i < ntx; i++)
            m1s += memcmp(txlog[i % TXMAX].f + 4, C.mac, 6) == 0 && txlog[i % TXMAX].f[0] == 0x08 && txlog[i % TXMAX].len == 32u + 99u;
        check(m1s == WA_HS_TRIES, "M1 sent WA_HS_TRIES times in all");
        e = last_sub(0xC0);
        check(e && memcmp(e->f + 4, C.mac, 6) == 0 && e->f[24] == 15 && e->wcid == WA_PLAIN, "then a deauthentication, reason 15, plain");
        check(wifi_ap_find_client(&wa.ap, C.mac) == 0, "the slot is free again");
    }

    /* 11. a channel change: kept at once, the AP restarts on it once it has rested */
    check(plat_wifi_ap_set_channel(0) == -1 && plat_wifi_ap_set_channel(14) == -1 && wn_st.ap.ch == 0u, "channel 0 and 14 refused");
    check(plat_wifi_ap_set_channel(11) == 0 && wn_st.ap.ch == 11u && wa.ch == 6u && wa.st == WA_UP, "channel 11 kept; the AP still on 6");
    tick(WA_RESTART_MS + 50u);
    run_until_up();
    check(wa.st == WA_UP && wa.ch == 11u && plat_wifi_ap_clients() == 0u && fm1_fake_ch == 11u, "the AP restarted on channel 11 (its clients gone)");
    tick(150);
    e = last_sub(0x80);
    check(e && e->f[38 + 7 + 10 + 2] == 11, "the beacon says channel 11");
    check(reg(0x6808) == 0u, "the old clients' keys were cleared");

    /* 12. a client again; MODE leaves AP: everyone told (reason 3), keys cleared, the AP stopped */
    client_init(&A, 0xA1, pmk, NAME, sizeof NAME);
    client_join(&A);
    check(A.sta.state == WIFI_STA_STATE_CONNECTED, "A joins after the restart");
    mark = ntx;
    plat_wifi_mode(WIFI_MODE_MAN);
    wifi_poll();
    check(wa.st == WA_OFF && wl.on == 0 && wl.ap == 0 && plat_wifi_addr() == 0u, "MODE left AP: the AP off, the service stopped");
    e = last_sub(0xC0);
    check(count_sub(mark, 0xC0) == 1u && e && memcmp(e->f + 4, A.mac, 6) == 0 && e->f[24] == 3 && e->wcid == WA_PLAIN, "the client was told (reason 3), plain");
    check(reg(0x6804) == 0u && reg(0x4020) == 0u, "its keys are out of the MAC");
    plat_wifi_mode(WIFI_MODE_OFF);
    mark = ntx;
    tick(300);
    check(ntx == mark, "no beacons while off");
    plat_wifi_mode(WIFI_MODE_AP);
    run_until_up();
    check(wa.st == WA_UP && wa.ch == 11u, "MODE AP again: the network kept comes up with no key job");

    /* 13. the key forgotten: the AP stops; the automatic name */
    plat_wifi_ap_forget();
    wifi_poll();
    check(wn_st.ap.set == 0u && wa.st == WA_OFF && plat_wifi_ap_state() == WIFI_AP_NOKEY && wn_st.ap.ch == 11u, "forget: off, NOKEY, the channel kept");
    tick(200);
    check(wa.st == WA_OFF, "and it stays off");
    check(plat_wifi_ap_set(0, 0, PW) == 0, "a new password with the automatic name");
    run_until_up();
    check(wa.st == WA_UP && wa.ssid_len == 8u && memcmp(wa.ssid, "FM1-4455", 8) == 0 && wn_st.ap.ssid_len == 0u,
          "the automatic name: FM1- and the last four digits of the address");

    /* 14. the radio does not start */
    wa_stop();
    wa.by_mode = 1;
    fm1_fake_up = 0;
    radio_ok = 0;
    for (i = 0; i < 50u && wa.st != WA_FAIL; i++) {
        ms += 10;
        wifi_poll();
    }
    check(wa.st == WA_FAIL && plat_wifi_ap_state() == WIFI_AP_FAILED, "the radio did not start: FAILED");
    radio_ok = 1;
    wa_stop();

    /* 15. AP MAN: off until started; a start / stop; another mode clears the switch; a typed password starts it */
    plat_wifi_mode(WIFI_MODE_AP_MAN);
    tick(300);
    check(wa.st == WA_OFF && plat_wifi_ap_state() == WIFI_AP_STOPPED, "AP MAN with a key, not started: nothing runs, STOPPED");
    check(plat_wifi_ap_run(1) == 0 && wa.user_on == 1, "plat_wifi_ap_run(1): started");
    run_until_up();
    check(wa.st == WA_UP && plat_wifi_ap_state() == WIFI_AP_UP, "... the AP comes up from the key kept");
    client_init(&A, 0xA1, wn_st.ap.pmk, wa.ssid, wa.ssid_len);
    client_join(&A);
    check(A.sta.state == WIFI_STA_STATE_CONNECTED && plat_wifi_ap_clients() == 1u, "... and a client joins it");
    mark = ntx;
    check(plat_wifi_ap_run(0) == 0, "plat_wifi_ap_run(0): stopped");
    wifi_poll();
    check(wa.st == WA_OFF && wl.on == 0 && plat_wifi_ap_state() == WIFI_AP_STOPPED && count_sub(mark, 0xC0) == 1u, "... the client told, the AP off, STOPPED");
    plat_wifi_ap_run(1);
    run_until_up();
    check(wa.st == WA_UP, "started again");
    plat_wifi_mode(WIFI_MODE_AUTO);
    wifi_poll();
    check(wa.user_on == 0 && wa.st == WA_OFF, "another mode: the switch off, the AP stopped");
    check(plat_wifi_ap_run(1) == -1, "plat_wifi_ap_run outside AP MAN: refused");
    plat_wifi_mode(WIFI_MODE_AP_MAN);
    wifi_poll();
    check(wa.st == WA_OFF && plat_wifi_ap_state() == WIFI_AP_STOPPED, "back in AP MAN: still stopped");
    wn_ap_forget(&wn_st);
    check(plat_wifi_ap_run(1) == -2 && plat_wifi_ap_state() == WIFI_AP_NOKEY, "no key kept: plat_wifi_ap_run(1) refused (-2), NOKEY");
    check(plat_wifi_ap_set(0, 0, PW) == 0 && wa.user_on == 1, "a typed password in AP MAN starts it");
    run_until_up();
    check(wa.st == WA_UP && wn_st.ap.set == 1u, "... the key made and kept, the AP up");

    /* 16. a password set while MODE is not AP: the key kept, nothing runs */
    plat_wifi_mode(WIFI_MODE_MAN);
    wifi_poll();
    check(plat_wifi_ap_set(NAME, sizeof NAME, "another-one") == 0, "a password with MODE MAN");
    run_until_up();
    {
        uint8_t k[32];
        wc_pmk("another-one", NAME, sizeof NAME, k);
        check(wa.st == WA_OFF && wn_st.ap.set == 1u && !memcmp(wn_st.ap.pmk, k, 32), "... its key kept, no access point");
    }

    /* 17. one not kept (MODE OFF only) */
    check(plat_wifi_ap_start(NAME, sizeof NAME, PW, 6) == -1, "plat_wifi_ap_start refused while MODE is MAN");
    plat_wifi_mode(WIFI_MODE_OFF);
    check(plat_wifi_ap_start(NAME, sizeof NAME, "short", 6) == -1 && plat_wifi_ap_start(NAME, sizeof NAME, PW, 14) == -1 &&
          plat_wifi_ap_start(NAME, 0, PW, 6) == -1, "plat_wifi_ap_start: a bad password, channel, name refused");
    check(plat_wifi_ap_start(NAME, sizeof NAME, PW, 1) == 0 && wa.by_mode == 0 && wa.st == WA_PMK, "plat_wifi_ap_start: accepted");
    run_until_up();
    check(wa.st == WA_UP && wa.ch == 1u && plat_wifi_ap_channel() == 1 && plat_wifi_ap_state() == WIFI_AP_UP, "up on channel 1");
    tick(500);
    check(wa.st == WA_UP, "MODE OFF does not stop it");
    plat_wifi_ap_stop();
    check(wa.st == WA_OFF && plat_wifi_ap_state() == WIFI_AP_OFF, "plat_wifi_ap_stop");
    check(plat_wifi_ap_start(NAME, sizeof NAME, PW, 3) == 0, "started again");
    run_until_up();
    plat_wifi_mode(WIFI_MODE_MAN);
    wifi_poll();
    check(wa.st == WA_OFF, "a mode chosen stops it");

    printf("wifi_apd: %d / %d checks passed\n", checks - fails, checks);
    return fails;
}
