/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David (hdavid), Optimist (github.com/w0ts/optimist); in fm1-base: tests/wifi/run.sh */
/* Link's node lent by the app (fm1.toml wifi_mem = "app": FM1B_WIFI_MEM, plat.h plat_link_mem; link/link_own.c) on the
 * host, device/wifi_own.c as the unity has it with link = true: Link wanted and an address, but no node: it does not
 * start, and every reader answers as off; the node lent: it starts at the next poll and runs (its packets sent); the
 * address gone: off, the node taken back (refused while it ran), poisoned under ASan, and every reader and writer then
 * leaves it alone; 50 such cycles with nothing left behind. Exit status: the number of failed checks. */
#include <stdio.h>
#include <string.h>
#include "plat.h"

static uint32_t ms = 5000u;
uint32_t plat_ms(void) { return ms; }
uint32_t plat_ticks24(void) { return ms * 24000u; }
const uint8_t *plat_chip_uid(void) { return 0; }
int plat_ble_state(void) { return BLE_READY; }
int plat_ble_radio(void) { return BLE_READY; }
void plat_ble_start(void) {}
enum { OBJ_WIFI = 2 };
int plat_store_load(uint32_t obj, void *dst, uint32_t max) { (void)obj, (void)dst, (void)max; return -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { (void)obj, (void)src, (void)len; return 0; }
#include "../../device/wifi_own.c"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LT_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define LT_ASAN 1
#endif
#ifdef LT_ASAN
#include <sanitizer/asan_interface.h>
#define LT_POISON(p, n) ASAN_POISON_MEMORY_REGION(p, n)
#define LT_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION(p, n)
#else
#define LT_POISON(p, n) ((void)(p), (void)(n))
#define LT_UNPOISON(p, n) ((void)(p), (void)(n))
#endif

static int fails, checks;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}
static uint32_t sent;
static int count_tx(const uint8_t *frame, uint32_t len, uint32_t wcid, uint32_t ack)
{
    (void)frame, (void)len, (void)wcid, (void)ack;
    sent++;
    return 0;
}
static uint8_t node[sizeof(struct lk_node) + 64u] __attribute__((aligned(8)));
static void readers(void)               /* every plat_link_* the app may call: off answers, nothing touched */
{
    (void)plat_link_peers(), (void)plat_link_mpb(), (void)plat_link_beat_at(1000000), (void)plat_link_time_at(1000000);
    (void)plat_link_playing(), (void)plat_link_play_beat();
    plat_link_set_tempo(500000);
    plat_link_set_playing(1);
}
int main(void)
{
    uint32_t i, ok_cycles = 0, lends = 0, takes = 0;
    const uint32_t IP = 0xC0A80117u;
    fm1_fake_tx = count_tx;
    check(plat_link_mem_bytes() >= sizeof(struct lk_node) && plat_link_mem_bytes() <= sizeof node, "the node's size");
    LT_POISON(node, sizeof node);       /* (the app's: not lent) */
    plat_link_enable(1);
    wl.on = 1, wl.ip.ip = IP;           /* (an address: DHCP bound, as wifi_own.c has it) */
    own_link_addr(IP);
    own_link_poll(own_us());
    check(!plat_link_on() && plat_link_beat_at(0) == -1, "no node: Link does not start, reads as off");
    readers();
    for (i = 0; i < 50u; i++) {
        uint32_t s0;
        LT_UNPOISON(node, sizeof node);
        memset(node, 0, sizeof node);
        lends += plat_link_mem(node) == 0;
        ms += 10u;
        own_link_poll(own_us());
        s0 = sent;
        ms += 1000u;
        own_link_poll(own_us());
        ms += 1000u;
        own_link_poll(own_us());
        if (!plat_link_on() || plat_link_mem(0) != -1)   /* (running: refused) */
            break;
        (void)s0;
        own_link_addr(0);               /* (the address gone: Wi-Fi off) */
        if (plat_link_on() || plat_link_mem(0) != 0)
            break;
        takes++;
        LT_POISON(node, sizeof node);
        readers();                      /* (after the give-back: nothing reads or writes it) */
        own_link_poll(own_us());
        ms += 500u;
        own_link_addr(IP);              /* (an address again, no node yet: not started) */
        own_link_poll(own_us());
        if (plat_link_on())
            break;
        ok_cycles++;
    }
    check(ok_cycles == 50u && lends == 50u && takes == 50u, "50 cycles: started once lent, taken back once off, never touched after");
    check(sent > 0u, "Link sent its packets while it ran");
    printf("link_mem: %d / %d checks passed\n", checks - fails, checks);
    return fails;
}
