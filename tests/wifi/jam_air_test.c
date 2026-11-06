/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Henri David */
/* MODE JAM on a model of the air: several units in one process, each with its own IP layer (wifi/wifi_ip.c as ad-hoc), Link
 * (link/link.c, unchanged) and group state (wifi/wifi_jam.c), its own clock (an offset and a crystal error) and its own main
 * loop (a frame received is stamped when a pass reads it, as device/wifi_own.c does). The air:
 *   one medium a channel: a frame takes its time at 1 Mb/s (192 us preamble, 8 us a byte), one at a time, after a random
 *   backoff; each receiver loses it with a chance; unicast is acknowledged and retried (the MAC's), a group frame is not;
 *   the MAC's filter by BSSID is left to ja_heard here (the same frames, more work).
 * What it shows: units find each other with nobody hosting, whenever they start and leave; a session forms and the phase
 * between units; loss; two groups on one channel stay apart. The pass times, the backoff and the crystal errors are
 * assumptions of this model, not measurements of an FM-1.
 * Exit status: the number of failed checks. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../wifi/wifi_ip.c"
#include "../../wifi/wifi_jam.c"
#include "../../link/link.c"

#define UNITS 6u
#define QMAX 64u
#define FRAME_MAX 640u
#define RETRIES 7u

static int fails;
static void check(int ok, const char *what)
{
    printf("%-108s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

static uint32_t rng = 0x1234567u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static void rand_bytes(uint8_t *o, uint8_t n)
{
    while (n--)
        *o++ = (uint8_t)rnd();
}

struct rxf {
    int64_t at;                          /* true us it is in the MAC's queue */
    uint32_t n;
    int8_t rssi;
    uint8_t f[FRAME_MAX];
};
struct unit {
    uint8_t on, grp;
    double ppm;
    int64_t offs;
    int64_t pass_at;                     /* true us of the next pass */
    uint32_t pass_lo, pass_hi;           /* a pass takes this many us */
    uint8_t mac[6];
    struct ja_net n;
    struct wifi_ip ip;
    struct lk_node k;
    struct rxf q[QMAX];
    uint32_t qr, qw;
    uint32_t tx_frames, tx_bytes, q_over;
};
static struct unit u[UNITS];
static int64_t t_now;                    /* true us */
static int64_t air_free[14];             /* a channel is busy until this */
static uint32_t loss_pct;
static uint64_t air_us[14];

static int64_t local_us(const struct unit *p) { return (int64_t)((double)t_now * (1.0 + p->ppm * 1e-6)) + p->offs; }

static void air_send(struct unit *s, const uint8_t *f, uint32_t n)
{
    int64_t start = t_now > air_free[s->n.ch] ? t_now : air_free[s->n.ch];
    uint32_t tries = (f[4] & 1u) ? 1u : RETRIES, t, i, us = 192u + 8u * (n + 4u);
    start += 50u + rnd() % 310u;         /* DIFS and a backoff of 0..15 slots */
    s->tx_frames++;
    s->tx_bytes += n;
    for (t = 0; t < tries; t++) {
        int heard = 0;
        for (i = 0; i < UNITS; i++) {
            struct unit *r = &u[i];
            if (r == s || !r->on || r->n.ch != s->n.ch || (!(f[4] & 1u) && memcmp(f + 4, r->mac, 6)))
                continue;
            if (rnd() % 100u < loss_pct)
                continue;
            heard = 1;
            if (r->qw - r->qr >= QMAX) {
                r->q_over++;
                continue;
            }
            {
                struct rxf *q = &r->q[r->qw++ % QMAX];
                q->at = start + us;
                q->n = n;
                q->rssi = -50;
                memcpy(q->f, f, n);
            }
        }
        start += us;
        air_us[s->n.ch] += us;
        if (f[4] & 1u)
            break;
        if (heard) {
            start += 10u + 304u;         /* SIFS and an ACK at 1 Mb/s */
            air_us[s->n.ch] += 314u;
            break;
        }
        start += 300u;                   /* (the ACK's timeout) */
    }
    air_free[s->n.ch] = start;
}
static void unit_send(uint32_t len)      /* (wifi_ip's frame: taken from the unit's own buffer below) */
{
    (void)len;
}
static int lk_tx(void *ctx, uint32_t ip, uint16_t port, const uint8_t *p, uint32_t n)
{
    struct unit *s = ctx;
    uint8_t b[FRAME_MAX];
    uint32_t len = wifi_ip_send_udp(&s->ip, LK_MCAST_PORT, ip, port, p, n, b);
    if (!len)
        return -1;
    air_send(s, b, len);
    return 0;
}
static void udp_rx(void *ctx, int64_t now, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *p, uint32_t n)
{
    struct unit *r = ctx;
    (void)dst_ip, (void)dst_port;
    lk_rx(&r->k, now, src_ip, src_port, p, n);
}
static uint32_t conflicts;
static void unit_rx(struct unit *r, const struct rxf *q, int64_t now)
{
    uint8_t b[FRAME_MAX];
    uint32_t tl = 0, ip;
    if (!ja_heard(&r->n, q->f, q->n, q->rssi, (uint32_t)(t_now / 1000)))
        return;
    ip = ja_sender_ip(q->f, q->n);
    if (ip == r->ip.ip) {
        uint8_t d[2];
        rand_bytes(d, 2);
        r->ip.ip = ja_addr(d);
        lk_set_addr(&r->k, r->ip.ip, LK_MCAST_PORT);
        conflicts++;
        return;
    }
    if (ip)
        wifi_ip_arp_learn(&r->ip, ip, q->f + 10);
    if (!wifi_ip_early_filter(&r->ip, q->f, q->n))
        return;
    if (wifi_ip_rx_frame(&r->ip, q->f, q->n, now, b, &tl) == 1 && tl)
        air_send(r, b, tl);
}
static void unit_pass(struct unit *p)
{
    int64_t now = local_us(p);
    uint32_t i;
    for (i = 0; i < 24u && p->qr != p->qw; i++) {
        const struct rxf *q = &p->q[p->qr % QMAX];
        if (q->at > t_now)
            break;
        unit_rx(p, q, now);
        p->qr++;
    }
    lk_poll(&p->k, now);
    p->pass_at = t_now + p->pass_lo + rnd() % (p->pass_hi - p->pass_lo + 1u);
}
static void unit_on(uint32_t i, uint32_t grp, double ppm, uint32_t lo, uint32_t hi)
{
    struct unit *p = &u[i];
    struct lk_cfg c;
    uint32_t ip;
    memset(p, 0, sizeof *p);
    p->grp = (uint8_t)grp;
    p->ppm = ppm;
    p->offs = (int64_t)(rnd() % 100000000u);
    p->pass_lo = lo;
    p->pass_hi = hi;
    p->pass_at = t_now + rnd() % 1000u;
    memcpy(p->mac, "\x02\x11\x22\x33", 4);
    p->mac[4] = (uint8_t)(0x40u + i);
    p->mac[5] = (uint8_t)(rnd() | 1u);
    ja_init(&p->n, p->mac, grp);
    ip = ja_addr(p->mac + 4);
    wifi_ip_ibss_init(&p->ip, p->mac, p->n.bssid, ip, rand_bytes);
    p->ip.send = unit_send;
    p->ip.udp_rx = udp_rx;
    p->ip.ctx = p;
    (void)wifi_ip_bind(&p->ip, LK_MCAST_PORT);
    wifi_ip_join(&p->ip, LK_MCAST_IP);
    c.ip = 0;
    c.port = LK_MCAST_PORT;
    c.send = lk_tx;
    c.ctx = p;
    lk_init(&p->k, &c, rnd() | 1u, 500000);
    p->on = 1;
    lk_enable(&p->k, 1, local_us(p));
    lk_set_addr(&p->k, ip, LK_MCAST_PORT);
}
static void unit_off(uint32_t i) { u[i].on = 0; }   /* (switched off: no bye-bye) */
static void reset(void)
{
    memset(u, 0, sizeof u);
    memset(air_free, 0, sizeof air_free);
    memset(air_us, 0, sizeof air_us);
    loss_pct = 0;
    conflicts = 0;
    t_now = 1000000;
}
static void run_ms(uint32_t ms)
{
    int64_t end = t_now + (int64_t)ms * 1000;
    while (t_now < end) {
        int64_t nx = end;
        uint32_t i;
        for (i = 0; i < UNITS; i++)
            if (u[i].on && u[i].pass_at < nx)
                nx = u[i].pass_at;
        t_now = nx;
        for (i = 0; i < UNITS; i++)
            if (u[i].on && u[i].pass_at <= t_now)
                unit_pass(&u[i]);
    }
}
static int same_session(uint32_t a, uint32_t b) { return !memcmp(u[a].k.sess, u[b].k.sess, 8); }
static int all_one(uint32_t n)           /* units 0..n-1 in one session, each with the others as peers */
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (!same_session(0, i) || lk_peers(&u[i].k) != n - 1u)
            return 0;
    return 1;
}
static uint32_t ms_until_one(uint32_t n, uint32_t limit)
{
    uint32_t t;
    for (t = 0; t < limit; t += 100u) {
        if (all_one(n))
            return t;
        run_ms(100);
    }
    return limit;
}
static int64_t pair_err(uint32_t a, uint32_t b)   /* |the beat of a - of b| at one true time, in us at the tempo */
{
    int64_t d = lk_beat_at(&u[a].k, local_us(&u[a])) - lk_beat_at(&u[b].k, local_us(&u[b]));
    d = d < 0 ? -d : d;
    return d * u[a].k.tl.mpb / 1000000;
}
static int cmp64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}
static void phase(const char *what, uint32_t n, uint32_t secs, int64_t limit_us)   /* the worst pair, sampled 20 times a second */
{
    static int64_t v[20 * 1200];
    uint32_t c = 0, s, a, b;
    char line[128];
    for (s = 0; s < secs * 20u && c < sizeof v / sizeof *v; s++) {
        int64_t w = 0;
        run_ms(50);
        for (a = 0; a < n; a++)
            for (b = a + 1u; b < n; b++) {
                int64_t e = pair_err(a, b);
                w = e > w ? e : w;
            }
        v[c++] = w;
    }
    qsort(v, c, sizeof *v, cmp64);
    snprintf(line, sizeof line, "%s: phase p50 %lld us, p99 %lld, max %lld (limit %lld)", what, (long long)v[c / 2u], (long long)v[c * 99u / 100u],
             (long long)v[c - 1u], (long long)limit_us);
    check(v[c - 1u] <= limit_us, line);
}

int main(void)
{
    uint8_t b[32], f[FRAME_MAX];
    uint32_t i, n, t;

    /* the names and the channels */
    ja_init(&u[0].n, (const uint8_t *)"\x02\x01\x02\x03\x04\x05", 1);
    check(u[0].n.name_len == 7u && !memcmp(u[0].n.name, "fm1-jam", 7) && u[0].n.ch == 1u, "group 1: fm1-jam, channel 1");
    ja_init(&u[0].n, (const uint8_t *)"\x02\x01\x02\x03\x04\x05", 2);
    check(u[0].n.name_len == 9u && !memcmp(u[0].n.name, "fm1-jam-2", 9) && u[0].n.ch == 6u, "group 2: fm1-jam-2, channel 6");
    ja_init(&u[0].n, (const uint8_t *)"\x02\x01\x02\x03\x04\x05", 4);
    check(u[0].n.ch == 1u, "group 4: channel 1 again");
    ja_init(&u[0].n, (const uint8_t *)"\x02\x01\x02\x03\x04\x05", 9);
    check(u[0].n.name_len == 9u && !memcmp(u[0].n.name, "fm1-jam-9", 9) && u[0].n.ch == 11u, "group 9: fm1-jam-9, channel 11");
    memcpy(b, u[0].n.bssid, 6);
    check(b[0] == 2u && !(b[0] & 1u) && b[5] == 9u, "the BSSID: locally administered, not a group address, the group in it");
    for (i = 0; i < 300u; i++) {
        uint32_t a;
        rand_bytes(b, 2);
        a = ja_addr(b);
        if ((a >> 16) != 0xA9FEu || (a >> 8 & 255u) < 1u || (a >> 8 & 255u) > 254u || (a & 255u) < 1u || (a & 255u) > 254u)
            break;
    }
    check(i == 300u, "the address: 169.254.1..254.1..254 whatever the bytes");

    /* the frames */
    reset();
    unit_on(0, 1, 0, 1000, 2000);
    n = wifi_ip_send_udp(&u[0].ip, LK_MCAST_PORT, LK_MCAST_IP, LK_MCAST_PORT, (const uint8_t *)"x", 1, f);
    check(n && f[0] == 0x08u && f[1] == 0u, "a data frame: open, neither ToDS nor FromDS");
    check(!memcmp(f + 4, "\x01\x00\x5e\x4c\x4e\x4b", 6), "addr1: Link's group's multicast address");
    check(!memcmp(f + 10, u[0].mac, 6) && !memcmp(f + 16, u[0].n.bssid, 6), "addr2 the sender, addr3 the group's BSSID");
    check(ja_sender_ip(f, n) == u[0].ip.ip, "the sender's IP read off the frame");
    unit_on(1, 1, 0, 1000, 2000);
    check(ja_heard(&u[1].n, f, n, -50, 0) && u[1].n.peer[0].n_data == 1u, "another unit of the group: heard, counted");
    check(!ja_heard(&u[0].n, f, n, -50, 0), "our own frame: not");
    unit_on(2, 2, 0, 1000, 2000);
    check(!ja_heard(&u[2].n, f, n, -50, 0) && !u[2].n.peer[0].used, "another group's: dropped, not counted");
    f[1] = 0x42u;
    check(!ja_heard(&u[1].n, f, n, -50, 0), "an access point's (FromDS): not ours");
    f[1] = 0x40u;
    check(!ja_heard(&u[1].n, f, n, -50, 0), "a protected one: not ours");
    /* an ARP who-has from a computer on the network */
    reset();
    unit_on(0, 1, 0, 1000, 2000);
    {
        uint8_t a[FRAME_MAX], r[FRAME_MAX];
        uint32_t tl = 0;
        memset(a, 0, 64);
        a[0] = 0x08u;
        memset(a + 4, 0xFF, 6);
        memcpy(a + 10, "\x02\x77\x77\x77\x77\x77", 6);
        memcpy(a + 16, u[0].n.bssid, 6);
        memcpy(a + 24, "\xAA\xAA\x03\x00\x00\x00\x08\x06", 8);
        a[33] = 1, a[34] = 8, a[36] = 6, a[37] = 4, a[39] = 1;
        memcpy(a + 40, a + 10, 6);
        a[46] = 169, a[47] = 254, a[48] = 9, a[49] = 9;
        a[56] = (uint8_t)(u[0].ip.ip >> 24), a[57] = (uint8_t)(u[0].ip.ip >> 16), a[58] = (uint8_t)(u[0].ip.ip >> 8), a[59] = (uint8_t)u[0].ip.ip;
        check(ja_heard(&u[0].n, a, 60, -50, 0) && wifi_ip_early_filter(&u[0].ip, a, 60) &&
              wifi_ip_rx_frame(&u[0].ip, a, 60, 0, r, &tl) == 1 && tl, "a who-has for our address: answered");
        check(r[1] == 0u && !memcmp(r + 4, a + 10, 6) && !memcmp(r + 16, u[0].n.bssid, 6), "the answer: an ad-hoc frame to the asker");
    }

    /* 2, 3, 4 units started together: nobody hosts, one session in seconds */
    for (n = 2; n <= 4u; n++) {
        char s[96];
        reset();
        for (i = 0; i < n; i++)
            unit_on(i, 1, i * 8.0 - 12.0, 1000, 9000);
        t = ms_until_one(n, 30000);
        snprintf(s, sizeof s, "%u units started together: one session after %u ms", n, t);
        check(t < 8000u, s);
        run_ms(5000);
        snprintf(s, sizeof s, "%u units", n);
        phase(s, n, 240, 3000);
        snprintf(s, sizeof s, "%u units: airtime %.1f %%", n, (double)air_us[1] * 100.0 / (double)t_now);
        check((double)air_us[1] / (double)t_now < 0.25, s);
        for (i = 0; i < n; i++)
            check(ja_count(&u[i].n, (uint32_t)(t_now / 1000)) == n - 1u, "  the unit counts the others");
    }

    /* a third unit comes late, one leaves, it plays on */
    reset();
    unit_on(0, 1, 5, 1000, 9000);
    unit_on(1, 1, -5, 1000, 9000);
    run_ms(6000);
    lk_set_playing(&u[0].k, 1, lk_beat_at(&u[0].k, local_us(&u[0])), local_us(&u[0]));
    run_ms(1000);
    check(lk_playing(&u[1].k), "start from one unit reaches the other");
    unit_on(2, 1, 10, 1000, 9000);
    t = ms_until_one(3, 30000);
    {
        char s[80];
        snprintf(s, sizeof s, "a late third unit is in the session after %u ms", t);
        check(t < 8000u, s);
    }
    unit_off(0);                         /* the founder goes */
    run_ms(8000);
    check(ja_count(&u[1].n, (uint32_t)(t_now / 1000)) == 1u && lk_peers(&u[1].k) == 1u, "the founder gone: the two left see each other only");
    phase("the two left", 2, 60, 3000);
    unit_on(0, 1, 5, 1000, 9000);
    t = ms_until_one(3, 30000);
    {
        char s[80];
        snprintf(s, sizeof s, "it comes back: in the session after %u ms", t);
        check(t < 8000u, s);
    }

    /* lost frames */
    for (n = 10; n <= 30u; n += 20u) {
        char s[80];
        reset();
        loss_pct = n;
        for (i = 0; i < 3u; i++)
            unit_on(i, 1, i * 10.0 - 10.0, 1000, 9000);
        t = ms_until_one(3, 60000);
        snprintf(s, sizeof s, "%u %% of the frames lost: one session after %u ms", n, t);
        check(t < 30000u, s);
        run_ms(5000);
        snprintf(s, sizeof s, "%u %% lost", n);
        phase(s, 3, 240, 4000);
    }

    /* two groups on one channel (1 and 4) */
    reset();
    unit_on(0, 1, 0, 1000, 9000);
    unit_on(1, 1, 0, 1000, 9000);
    unit_on(2, 4, 0, 1000, 9000);
    unit_on(3, 4, 0, 1000, 9000);
    run_ms(10000);
    check(u[0].n.ch == u[2].n.ch && u[0].ip.ip != 0, "groups 1 and 4 share channel 1");
    check(lk_peers(&u[0].k) == 1u && lk_peers(&u[2].k) == 1u && same_session(0, 1) && same_session(2, 3) && !same_session(0, 2), "they stay two sessions");
    check(ja_count(&u[0].n, (uint32_t)(t_now / 1000)) == 1u, "each counts its own only");

    /* two units with one address: the second that hears it draws another */
    reset();
    unit_on(0, 1, 0, 1000, 9000);
    unit_on(1, 1, 0, 1000, 9000);
    u[1].ip.ip = u[0].ip.ip;
    lk_set_addr(&u[1].k, u[1].ip.ip, LK_MCAST_PORT);
    run_ms(6000);
    check(conflicts && u[0].ip.ip != u[1].ip.ip && all_one(2), "one address on two units: drawn again, one session");

    printf("%d failed\n", fails);
    return fails;
}
