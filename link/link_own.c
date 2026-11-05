/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Henri David */
/* Ableton Link on fm1-base's own Wi-Fi (fm1.toml link = true: FM1B_LINK, with wifi_stack = "own"): link.c, a C port of
 * the tempo / beat / start-stop part of Ableton's Link library (GPL-2.0-or-later, Copyright 2016 Ableton AG), as a peer
 * on the network the FM-1 joined. An opt-in part under GPL-3.0-only: a firmware built with it is
 * GPL-3.0-only as a whole (tools/build.py refuses link = true unless fm1.toml says licence = "GPL-3.0-only" or
 * "GPL-3.0-or-later"; LICENSING.md).
 *
 * Link runs from the main loop (wifi_own.c wl_poll: own_link_poll), over the IP layer's UDP: the multicast group
 * 224.76.78.75 and port 20808 for discovery and measurement. Times are Link's host clock in microseconds,
 * plat_link_now (plat_ticks24 on an FM-1, the simulator's own time on the host). The app reads the session with
 * plat_link_beat_at / plat_link_time_at (a MIDI clock: a tick at the time of each 1/24 beat), and changes it with
 * plat_link_set_tempo / plat_link_set_playing. Part of wifi_own.c (the unity build). */
#include "link.c"

#if FM1B_WIFI_MEM
/* the session's node (1.8 KB) in the app's memory (fm1.toml wifi_mem = "app": plat.h plat_link_mem): lent before Link
 * can run, taken back once it no longer does (lko.on 0); every reader checks lko.on, which is never 1 without it */
static struct lk_node *own_lkp;
#define OWN_LK own_lkp
#else
static struct lk_node own_lk;
#define OWN_LK (&own_lk)
#endif
static struct {
    uint8_t want, on;                             /* the app wants Link; it runs (an address and the port) */
    int32_t mpb;                                  /* the tempo it starts a session of its own with (us a beat) */
    uint32_t seed;
} lko = {0, 0, 500000, 0};

/* the clock: a snapshot of own_us's reference (wifi_own.c), for a reader in an interrupt (plat_link_now): two slots,
 * the main loop writes the other one, then makes it current */
#ifndef PLAT_HOST
static struct { int64_t us; uint32_t t; } lk_snap[2];
static volatile uint8_t lk_snap_k;
static void lk_snap_put(void)
{
    uint32_t k = lk_snap_k ^ 1u;
    lk_snap[k].us = own_us_ref;
    lk_snap[k].t = own_t_ref - own_rem;
    __asm__ volatile("" ::: "memory");           /* (the slot written before it is made current) */
    lk_snap_k = (uint8_t)k;
}
#endif
int64_t plat_link_now(void)
{
#ifdef PLAT_HOST
    return own_us();
#else
    uint32_t k = lk_snap_k;
    return lk_snap[k].us + (int32_t)(plat_ticks24() - lk_snap[k].t) / (int32_t)PLAT_TICKS_PER_US;
#endif
}
uint32_t plat_link_t24(int64_t us)                /* plat_ticks24 at Link time us (within a minute of now) */
{
#ifdef PLAT_HOST
    return plat_ticks24() + (uint32_t)(int32_t)(us - own_us()) * PLAT_TICKS_PER_US;
#else
    uint32_t k = lk_snap_k;
    return lk_snap[k].t + (uint32_t)(int32_t)(us - lk_snap[k].us) * PLAT_TICKS_PER_US;
#endif
}

static int own_link_send(void *ctx, uint32_t ip, uint16_t port, const uint8_t *p, uint32_t n)
{
    (void)ctx;
    return plat_net_udp_send(LK_MCAST_PORT, ip, port, p, n);
}
static void own_link_rx(void *ctx, uint32_t src_ip, uint16_t src_port, const uint8_t *p, uint32_t n, int64_t at_us)
{
    (void)ctx;
    if (lko.on)
        lk_rx(OWN_LK, at_us, src_ip, src_port, p, n);
}
static void own_link_start(uint32_t ip)           /* an address and Link wanted: a session of our own, then the others' */
{
    struct lk_cfg c;
    int64_t now = own_us();
    c.ip = 0;
    c.port = LK_MCAST_PORT;
    c.send = own_link_send;
    c.ctx = 0;
#if FM1B_WIFI_MEM
    if (!own_lkp)
        return;                                   /* (no node lent yet: started once it is, own_link_poll) */
#endif
    if (!lko.seed)
        own_rand((uint8_t *)&lko.seed, 4);
    lk_init(OWN_LK, &c, lko.seed, lko.mpb);
    lk_enable(OWN_LK, 1, now);
    lk_set_addr(OWN_LK, ip, LK_MCAST_PORT);
    lko.on = 1;
}
static void own_link_stop(void)
{
    if (lko.on) {
        lk_enable(OWN_LK, 0, own_us());          /* (bye-bye, while the address is still there) */
        lko.on = 0;
    }
}
static void own_link_addr(uint32_t ip)            /* wifi_own.c: DHCP bound (or the AP up), or the address gone (0) */
{
    if (!ip) {
        if (lko.on)
            lk_set_addr(OWN_LK, 0, LK_MCAST_PORT);
        lko.on = 0;
        return;
    }
    if (lko.want && !lko.on)
        own_link_start(ip);
    else if (lko.on)
        lk_set_addr(OWN_LK, ip, LK_MCAST_PORT);
}
static void own_link_poll(int64_t now)
{
#ifndef PLAT_HOST
    lk_snap_put();
#endif
#if FM1B_WIFI_MEM
    if (lko.want && !lko.on && own_lkp && wl.on && wl.ip.ip)
        own_link_start(wl.ip.ip);                 /* (the node lent after the address came) */
#endif
    if (lko.on)
        lk_poll(OWN_LK, now);
}
uint32_t plat_link_mem_bytes(void) { return FM1B_WIFI_MEM ? (uint32_t)(sizeof(struct lk_node) + 3u) / 4u * 4u : 0u; }
int plat_link_mem(void *p)                        /* (the main loop; the app's interrupts off around it) */
{
#if FM1B_WIFI_MEM
    if ((!p && lko.on) || ((uintptr_t)p & 3u))
        return -1;                                /* (running: plat_link_enable(0) or the Wi-Fi off first) */
    own_lkp = (struct lk_node *)p;
    return 0;
#else
    return p ? -1 : 0;
#endif
}

void plat_link_enable(int on)
{
    if (on && !lko.want) {
        lko.want = 1;
        (void)plat_net_udp_open(LK_MCAST_PORT, own_link_rx, 0);
        plat_net_join(LK_MCAST_IP);
        if (wl.on && wl.ip.ip)
            own_link_start(wl.ip.ip);
    } else if (!on && lko.want) {
        own_link_stop();
        lko.want = 0;
        plat_net_udp_close(LK_MCAST_PORT);
        plat_net_join(0);
    }
}
int plat_link_on(void) { return lko.on; }
uint32_t plat_link_peers(void) { return lko.on ? lk_peers(OWN_LK) : 0u; }
int32_t plat_link_mpb(void) { return lko.on ? lk_mpb(OWN_LK) : lko.mpb; }
int64_t plat_link_beat_at(int64_t us) { return lko.on ? lk_beat_at(OWN_LK, us) : -1; }
int64_t plat_link_time_at(int64_t ub) { return lko.on ? lk_time_at(OWN_LK, ub) : 0; }
int plat_link_playing(void) { return lko.on && lk_playing(OWN_LK); }
int64_t plat_link_play_beat(void) { return lko.on ? lk_play_beat(OWN_LK) : 0; }
int64_t plat_link_phase(int64_t ub, int64_t quantum_ub) { return lk_phase(ub, quantum_ub); }
void plat_link_set_tempo(int32_t mpb)
{
    if (mpb < LK_MPB_MIN || mpb > LK_MPB_MAX)
        return;
    lko.mpb = mpb;
    if (lko.on)
        lk_set_tempo(OWN_LK, mpb, own_us());
}
void plat_link_set_playing(int play)
{
    int64_t now;
    if (!lko.on)
        return;
    now = own_us();
    lk_set_playing(OWN_LK, play != 0, lk_beat_at(OWN_LK, now), now);
}
