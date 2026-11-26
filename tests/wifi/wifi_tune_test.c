/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David (hdavid), Optimist (github.com/w0ts/optimist); in fm1-base: tests/wifi/run.sh */
/* The own stack's tuning (wifi/wifi_tune.c, device/wifi_own_tune.c) on the host: device/wifi_own.c as the unity has it,
 * the radio host/wifi_fake/fm1_wifi.h with its MAC and BBP writes recorded and a flash that holds a VM. Covers, for each
 * switch, its reference's numbers and that off writes nothing new:
 *   (a) the record's check; the VM walk (the last valid 187 wins, a failing check ends the log, no magic no VM); the user's
 *       FM-1's own 187 (Optimist tests/ble_vm_fm1_e6000.bin at 0x0E8000: TX I gain 1018, I phase 27, DC -4 / +4, RX Q gain
 *       252, RX Q phase -3) read and put in at the Wi-Fi side's start, the start's values back when switched off, in again
 *       when on (the VM read once), nothing with a damaged record or none;
 *   (b) 0x1400: the station's 0x17F97 once associated, 0x1 back while scanning and in MODE AP, nothing written while off;
 *   (c) JieLi's steps (34 above -36 dBm, 43, 51, 57, 66) over a scripted signal, the hysteresis at each edge, 66 after
 *       220 ms with nothing from the AP, at a join's end and when switched off; one write a change;
 *   (f) the frames' rates (unicast data with an ACK at CCK 2 Mb/s; management, EAPOL, group-addressed, no-ACK and anything
 *       before the join at 1 Mb/s), the MAC's fallback table (JieLi's 0xEDCBA988 / 0x2100), the fall to 1 Mb/s after 3
 *       never-acknowledged frames in a second (from the MAC's counters), the hold, its doubling and the way back.
 * Exit status: the number of failed checks. */
#include <stdio.h>
#include <string.h>
#include "plat.h"

/* ---- the platform, as wifi_own.c uses it */
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

static int fails, checks;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}

/* ---- the recorded radio */
static uint32_t regs[0x8000 / 4], n_mac, n_bbp, n_flash;
static void reg_wr(uint32_t r, uint32_t v) { n_mac++; if (r < sizeof regs) regs[r / 4] = v; }
static void bbp_wr(uint32_t w, uint32_t e, uint32_t v) { (void)w, (void)e, (void)v; n_bbp++; }
/* the user's FM-1's VM records 106, 107, 187 as its dump has them (Optimist tests/ble_vm_fm1_e6000.bin, 0x0E8000) */
static const uint8_t REC187[68] = {
    0xfa, 0x03, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0xff, 0xff, 0xff,
    0x04, 0x00, 0x00, 0x00, 0x46, 0x2f, 0xff, 0xfc, 0x00, 0xfd, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x00, 0x00,
    0xff, 0xff, 0x7f, 0x39, 0xff, 0xff, 0x7f, 0x39, 0x50, 0x00, 0xea, 0x02, 0x4a, 0x00, 0xf0, 0x02, 0x08, 0x00, 0x9e, 0x02,
    0x2c, 0x02, 0x5c, 0x02, 0x8f, 0x5a, 0x00, 0x00};
static const uint8_t VM_HEAD[25] = {0x55, 0xaa, 0xaa, 0x55, 0x91, 0x6a, 0x20, 0x00, 0x0b, 0x0b,                  /* 106 */
                                    0x2c, 0x6b, 0x70, 0x00, 0x01, 0x07, 0x04, 0x07, 0x0b, 0x01, 0x07,            /* 107 */
                                    0x22, 0xbb, 0x40, 0x04};                                                       /* 187 */
static uint8_t flash[WT_VM_SIZE];
static void vm_make(int bad187)
{
    memset(flash, 0xFF, sizeof flash);
    memcpy(flash, VM_HEAD, sizeof VM_HEAD);
    memcpy(flash + sizeof VM_HEAD, REC187, sizeof REC187);
    if (bad187)
        flash[sizeof VM_HEAD + 64] ^= 1u;        /* (the record's own check broken, its header's still right) */
}
static int flash_rd(uint32_t off, uint8_t *dst, uint32_t n)
{
    n_flash++;
    if (off < WT_VM_AREA || off + n > WT_VM_AREA + sizeof flash)
        return -1;
    memcpy(dst, flash + (off - WT_VM_AREA), n);
    return 0;
}
static int ctx_rd(void *ctx, uint32_t off, uint8_t *dst, uint32_t n) { (void)ctx; return flash_rd(off, dst, n); }

static void restart(uint32_t mask)               /* power-on: the tuning's switches as given, the radio not started */
{
    memset(&wt, 0, sizeof wt);
    memset(&wc, 0, sizeof wc);
    memset(&wl, 0, sizeof wl);
    memset(&oq, 0, sizeof oq);
    fm1_fake_up = 0;
    fm1_fake_agc = FM1_WIFI_AGC_START;
    fm1_fake_cal_calls = 0;
    fm1_fake_cal_last = 0;
    own_store_ok = 0;
    own_store_load();
    wn_st.rsv = wt_mask_keep(mask);
    memset(regs, 0, sizeof regs);
}

/* ---- the pure part */
static void t_pure(void)
{
    uint8_t r[68], out[68];
    static const struct { int rssi; unsigned g; } J[] = {{-20, 34}, {-36, 43}, {-35, 34}, {-46, 51}, {-45, 43}, {-60, 57},
                                                         {-59, 51}, {-76, 66}, {-75, 57}, {-95, 66}};
    unsigned i;
    memcpy(r, REC187, 68);
    check(wt_187_ok(r), "187: the FM-1's record's check holds");
    r[3] ^= 1u;
    check(!wt_187_ok(r), "187: a changed byte fails");
    memset(r, 0, 68);
    check(!wt_187_ok(r), "187: a check of 0 is no record");
    check(wt_mask_from(0, WT_ALL) == WT_ALL && wt_mask_from(wt_mask_keep(0), WT_ALL) == 0 &&
          wt_mask_from(wt_mask_keep(WT_AGC | WT_RATE), WT_ALL) == (WT_AGC | WT_RATE), "the mask: default, kept 0, kept c+f");
    vm_make(0);
    check(wt_vm_187(ctx_rd, 0, WT_VM_AREA, WT_VM_SIZE, out) && !memcmp(out, REC187, 68), "VM: the FM-1's 187 found");
    check((uint32_t)(out[0] | out[1] << 8) == 1018u && out[4] == 27 && (int8_t)out[16] == -4 && out[20] == 4 &&
          out[27] == 252 && (int8_t)out[29] == -3, "VM: I gain 1018, I phase 27, DC -4 / +4, RX Q gain 252, phase -3");
    {   /* a second 187 after it, the last valid one wins; then one with a bad header check: the log ends there */
        uint32_t at = sizeof VM_HEAD + 68;
        memcpy(flash + at, VM_HEAD + 21, 4);
        memcpy(flash + at + 4, REC187, 68);
        flash[at + 4 + 4] = 28;                 /* (I phase 28: its CRC and the header's check made again) */
        {
            uint32_t c = wt_crc16(0, flash + at + 4, 64);
            flash[at + 4 + 64] = (uint8_t)c, flash[at + 4 + 65] = (uint8_t)(c >> 8);
            flash[at] = (uint8_t)wt_crc16(0, flash + at + 4, 68);
        }
        check(wt_vm_187(ctx_rd, 0, WT_VM_AREA, WT_VM_SIZE, out) && out[4] == 28, "VM: the last valid 187 wins");
        flash[at] ^= 0xFFu;
        check(wt_vm_187(ctx_rd, 0, WT_VM_AREA, WT_VM_SIZE, out) && out[4] == 27, "VM: a failing check ends the log");
    }
    vm_make(1);
    check(!wt_vm_187(ctx_rd, 0, WT_VM_AREA, WT_VM_SIZE, out), "VM: a 187 whose own check fails is none");
    memset(flash, 0xFF, sizeof flash);
    check(!wt_vm_187(ctx_rd, 0, WT_VM_AREA, WT_VM_SIZE, out), "VM: no magic, no VM");
    for (i = 0; i < sizeof J / sizeof *J; i++) {
        char b[64];
        snprintf(b, sizeof b, "AGC: JieLi's step at %d dBm is %u", J[i].rssi, J[i].g);
        check(wt_agc_step(J[i].rssi) == J[i].g, b);
    }
    {   /* a signal walking down and up through the edges: each step WT_AGC_HYST dB past its edge */
        static const struct { int rssi; unsigned g; } W[] = {{-30, 34}, {-37, 34}, {-38, 34}, {-39, 43}, {-48, 43}, {-49, 51},
            {-62, 51}, {-63, 57}, {-78, 57}, {-79, 66}, {-73, 66}, {-72, 57}, {-57, 57}, {-56, 51}, {-43, 51}, {-42, 43},
            {-33, 43}, {-32, 34}};
        uint32_t g = 66;
        for (i = 0; i < sizeof W / sizeof *W; i++) {
            char b[64];
            g = wt_agc_next(g, W[i].rssi);
            snprintf(b, sizeof b, "AGC: hysteresis, %d dBm -> %u (got %u)", W[i].rssi, W[i].g, (unsigned)g);
            check(g == W[i].g, b);
        }
    }
    {   /* the rate: 3 lost in a second falls back for 10 s, a fall within 30 s of coming back holds 20 s */
        struct wt_rate rt;
        memset(&rt, 0, sizeof rt);
        wt_rate_reset(&rt, 0);
        wt_rate_second(&rt, 1000, 2);
        check(!rt.slow, "rate: 2 lost in a second: still fast");
        wt_rate_second(&rt, 2000, 3);
        check(rt.slow && rt.falls == 1u, "rate: 3 lost: 1 Mb/s");
        wt_rate_second(&rt, 11000, 0);
        check(rt.slow, "rate: held 10 s (9 s in: still slow)");
        wt_rate_second(&rt, 12000, 0);
        check(!rt.slow, "rate: fast again after 10 s");
        wt_rate_second(&rt, 15000, 5);
        check(rt.slow && rt.k == 1u && rt.t_until == 35000u, "rate: fell again 3 s after: held 20 s");
        wt_rate_second(&rt, 35000, 0);
        wt_rate_second(&rt, 70000, 4);
        check(rt.slow && rt.k == 0u && rt.t_until == 80000u, "rate: a fall 35 s after: 10 s again");
    }
}

/* ---- frames, built in wifi_txb */
static uint32_t frame(uint8_t fc0, int group, int eapol)
{
    uint8_t *f = own_frame();
    memset(f, 0, 64);
    f[0] = fc0;
    f[1] = 0x01;                                 /* (ToDS) */
    memset(f + 4, 0x22, 6);
    if (group)
        f[4] = 0x01;
    if (eapol)
        f[24 + 6] = 0x88, f[24 + 7] = 0x8E;
    else
        f[24 + 6] = 0x08, f[24 + 7] = 0x00;
    return 60;
}
static uint32_t sent_w0(uint8_t fc0, int group, int eapol, uint32_t ack)
{
    own_tx(frame(fc0, group, eapol), 1u, ack);
    return fm1_fake_w0;
}

static void t_cal(void)
{
    uint32_t n0, f0;
    vm_make(0);
    restart(WT_ALL);
    check(own_radio_up() == 1 && wt.up, "(a) the Wi-Fi side up");
    check(wt.cal == 1u && wt.cal_in && fm1_fake_cal_calls == 1u && fm1_fake_cal_last == wt.x187 &&
          !memcmp(wt.x187, REC187, 68), "(a) the stored 187 put in at the start");
    f0 = n_flash;
    plat_wifi_tune_set(WT_ALL & ~WT_CAL);
    check(fm1_fake_cal_calls == 2u && fm1_fake_cal_last == 0 && !wt.cal_in, "(a) off: the start's values back");
    plat_wifi_tune_set(WT_ALL);
    check(fm1_fake_cal_calls == 3u && fm1_fake_cal_last == wt.x187 && wt.cal_in && n_flash == f0, "(a) on again, the VM not read again");
    vm_make(1);
    restart(WT_ALL);
    (void)own_radio_up();
    check(wt.cal == 2u && !wt.cal_in && fm1_fake_cal_calls == 0u, "(a) a damaged 187: nothing put in");
    vm_make(0);
    restart(WT_ALL & ~WT_CAL);
    f0 = n_flash;
    (void)own_radio_up();
    check(wt.cal == 0u && fm1_fake_cal_calls == 0u && n_flash == f0, "(a) off at the start: no VM read, nothing put in");
    /* all four off: the radio's start writes what it wrote before the tuning, nothing in the BBP */
    restart(0);
    n0 = n_mac;
    f0 = n_bbp;
    (void)own_radio_up();
    wc.st = WC_UP;
    wt_heard(-30);
    wt_poll();
    ms += 2000u;
    wt_poll();
    check(n_mac == n0 && n_bbp == f0 && regs[FM1_WIFI_LG_FBK0 / 4] == 0u, "all off: no MAC or BBP write of the tuning's");
    check(sent_w0(0x08, 0, 0, 1) == FM1_WIFI_RATE_CCK1, "all off: data at 1 Mb/s");
}

static void t_filter_agc(void)
{
    uint32_t n0, b0;
    restart(WT_ALL);
    (void)own_radio_up();
    wc.st = WC_SCAN;
    wt_poll();
    check(regs[0x1400 / 4] == 0u && !wt.filt, "(b) scanning: nothing written by the tuning");
    wc.st = WC_HS;
    wt_poll();
    check(regs[0x1400 / 4] == FM1_WIFI_RX_FILTER_STA && wt.filt, "(b) associated: 0x17F97");
    wc.st = WC_UP;
    n0 = n_mac;
    wt_poll();
    check(n_mac == n0, "(b) joined: written once");
    plat_wifi_tune_set(WT_ALL & ~WT_FILT);
    wt_poll();
    check(regs[0x1400 / 4] == FM1_WIFI_RX_FILTER_ALL && !wt.filt, "(b) off while joined: everything in");
    plat_wifi_tune_set(WT_ALL);
    wt_poll();
    check(regs[0x1400 / 4] == FM1_WIFI_RX_FILTER_STA, "(b) on again");
    wl.ap = 1, wl.on = 1;
    wt_poll();
    check(regs[0x1400 / 4] == FM1_WIFI_RX_FILTER_ALL, "(b) MODE AP: everything in");
    wl.ap = 0, wl.on = 0;
    wc.st = WC_SCAN;
    wt_poll();
    check(regs[0x1400 / 4] == FM1_WIFI_RX_FILTER_ALL, "(b) scanning again: everything in");

    /* (c): a signal walking, the AP's frames at 10 a second */
    restart(WT_ALL);
    (void)own_radio_up();
    wc.st = WC_UP;
    {
        static const struct { int rssi; unsigned g; } S[] = {{-30, 34}, {-50, 51}, {-62, 51}, {-65, 57}, {-82, 66}, {-40, 43}};
        unsigned i, k;
        for (i = 0; i < sizeof S / sizeof *S; i++) {
            char b[64];
            b0 = n_bbp;
            for (k = 0; k < 40u; k++) {          /* 4 s of beacons: the mean settles */
                ms += 100u;
                wt_heard((int8_t)S[i].rssi);
                wt_poll();
            }
            snprintf(b, sizeof b, "(c) %d dBm: gain %u (got %u)", S[i].rssi, S[i].g, (unsigned)fm1_fake_agc);
            check(fm1_fake_agc == S[i].g && wt.gain == S[i].g, b);
            check(n_bbp - b0 <= 2u * 3u, "(c) a few writes a change at most (one write = 1 window write here)");
        }
    }
    ms += WT_AGC_IDLE_MS;
    wt_poll();
    check(fm1_fake_agc == 66u, "(c) the AP quiet 220 ms: 66");
    wt_heard(-30), ms += 100u, wt_heard(-30), wt_poll();
    plat_wifi_tune_set(WT_ALL & ~WT_AGC);
    wt_poll();
    check(fm1_fake_agc == 66u, "(c) off: 66");
    plat_wifi_tune_set(WT_ALL);
    wt_heard(-30), wt_poll();
    check(fm1_fake_agc < 66u, "(c) on again: down from 66");
    wc.st = WC_SCAN;
    wt_poll();
    check(fm1_fake_agc == 66u, "(c) the join over: 66");
}

static void t_rate(void)
{
    uint32_t i;
    restart(WT_ALL);
    (void)own_radio_up();
    check(regs[FM1_WIFI_LG_FBK0 / 4] == 0xEDCBA988u && regs[FM1_WIFI_LG_FBK1 / 4] == 0x2100u, "(f) the MAC's fallback table, JieLi's");
    wc.st = WC_AUTH;
    check(sent_w0(0x08, 0, 0, 1) == FM1_WIFI_RATE_CCK1, "(f) before the join: 1 Mb/s");
    wc.st = WC_UP;
    wt_poll();
    check(sent_w0(0x08, 0, 0, 1) == FM1_WIFI_RATE_CCK2 && fm1_fake_w0 == 0x000100B0u, "(f) unicast data: CCK 2 Mb/s (0x000100B0)");
    check(sent_w0(0x88, 0, 0, 1) == FM1_WIFI_RATE_CCK2, "(f) QoS data: 2 Mb/s");
    check(sent_w0(0xC0, 0, 0, 1) == FM1_WIFI_RATE_CCK1, "(f) management: 1 Mb/s");
    check(sent_w0(0x08, 0, 1, 1) == FM1_WIFI_RATE_CCK1, "(f) EAPOL: 1 Mb/s");
    check(sent_w0(0x08, 1, 0, 1) == FM1_WIFI_RATE_CCK1, "(f) group-addressed: 1 Mb/s");
    check(sent_w0(0x08, 0, 0, 0) == FM1_WIFI_RATE_CCK1, "(f) no ACK: 1 Mb/s");
    {
        struct plat_wifi_quality q;
        check(plat_wifi_quality(&q) && q.tx_kbps == 2000u, "(f) plat_wifi_quality says 2000 kb/s");
    }
    /* the air loses every frame at 2 Mb/s: 3 in a second never acknowledged -> 1 Mb/s */
    fm1_fake_lost_fast_pct = 100;
    for (i = 0; i < 25u && !wt.rate.slow; i++) {
        ms += 100u;
        (void)sent_w0(0x08, 0, 0, 1);
        wt_poll();
    }
    check(wt.rate.slow && wt.rate.falls == 1u, "(f) lost at 2 Mb/s: fell back within 2.5 s");
    check(sent_w0(0x08, 0, 0, 1) == FM1_WIFI_RATE_CCK1, "(f) fallen back: data at 1 Mb/s");
    fm1_fake_lost_fast_pct = 0;
    for (i = 0; i < 120u && wt.rate.slow; i++) {
        ms += 100u;
        (void)sent_w0(0x08, 0, 0, 1);
        wt_poll();
    }
    check(!wt.rate.slow && i >= 90u, "(f) back to 2 Mb/s after the 10 s hold");
    plat_wifi_tune_set(WT_ALL & ~WT_RATE);
    check(sent_w0(0x08, 0, 0, 1) == FM1_WIFI_RATE_CCK1, "(f) off: 1 Mb/s");
    wl.ap = 1, wl.on = 1, wc.st = WC_IDLE;
    plat_wifi_tune_set(WT_ALL);
    check(sent_w0(0x08, 0, 0, 1) == FM1_WIFI_RATE_CCK2, "(f) MODE AP: a client's data at 2 Mb/s");
}

int main(void)
{
    wifi_sta_set_test_mac_wr(reg_wr);
    fm1_fake_bbp_wr = bbp_wr;
    fm1_fake_flash = flash_rd;
    t_pure();
    t_cal();
    t_filter_agc();
    t_rate();
    printf("%d checks, %d failed\n", checks, fails);
    return fails;
}
