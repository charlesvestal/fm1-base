/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Copyright (C) 2026 Henri David */
/* The update loader (bsp/loader/ldr_core.c) between app slots of two sizes (tools/build.py app_end: FL_APP_END,
 * LDR_SLOT_SWITCH, LDR_CLEAR_END), built here with the NEW package's slot: the "device" flash starts as OLD's flash.bin
 * with data above it (0xA5), a fake host serves NEW (tests/ldr_test.c's). Going up (OLD's area ends below this one's):
 * [FL_APP_END, LDR_CLEAR_END) erased first, the rest of the data kept; going down: the old app's code above FL_APP_END
 * erased, the data above the old end kept; then the same package again: nothing erased.
 *   cc -DFL_APP_END=0xCE000u -DLDR_SLOT_SWITCH=1 -DLDR_CLEAR_END=0xDA000u -o ldr_slot_test tests/ldr_slot_test.c
 *   ldr_slot_test OLD.fwsc NEW.fwsc OLD_END
 * With LDR_STEPS (an app's app_steps: tools/build.py ldr_step_flags), --steps: the switch matrix of those steps;
 * --range END...: the range erased after each installed END, for the app's own test of its rule */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FL_APP_END
#define FL_APP_END 0x93000u
#endif
#define FELUCCA_ID "ota-FM-1_900"
#define FELUCCA_OTA_DRYRUN 0

static uint8_t nor[0x100000], *logical;
static size_t logical_len;
static uint8_t rx[1024];
static uint32_t rx_len, rx_full, now_ms, requests, erases, bad_range, record_cleared, f0_asked;

static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8;
        while (nb >= 7) { out[o++] = acc & 0x7F; acc >>= 7; nb -= 7; }
    }
    if (nb) out[o++] = acc & 0x7F;
    return o;
}
static uint32_t unpack7h(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 7;
        if (nb >= 8) { out[o++] = (uint8_t)acc; acc >>= 8; nb -= 8; }
    }
    return o;
}

static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    uint8_t u[64], m[600];
    uint32_t addr, len, i, s = 0;
    unpack7h(p + 1, n - 2, u);
    if (u[2] == 0x11) return 0;
    addr = u[7] | u[8] << 8 | u[9] << 16 | (uint32_t)u[10] << 24;
    len = u[11] | u[12] << 8 | u[13] << 16;
    requests++;
    memcpy(m, "\x00\x59\x30", 3);
    m[3] = (uint8_t)(len + 8); m[4] = (uint8_t)((len + 8) >> 8); m[5] = 0; m[6] = 0;
    memcpy(m + 7, &addr, 4);
    m[11] = (uint8_t)len; m[12] = (uint8_t)(len >> 8); m[13] = 0;
    if (addr >= 0xE0000000u) {
        f0_asked += addr == 0xF0000000u;
        memset(m + 14, 0, len);
        memcpy(m + 14, "success", 8);
    } else {
        if (addr + len > logical_len) { printf("read past the package %#x\n", addr); exit(1); }
        memcpy(m + 14, logical + addr, len);
    }
    for (i = 6; i < 14 + len; i++) s += m[i];
    m[14 + len] = (uint8_t)~s;
    rx_len = pack7(m, 15 + len, rx);
    rx_full = 1;
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n) { if (!rx_full) return 0; *p = rx; *n = rx_len; return 1; }
static void ota_frame_done(void) { rx_full = 0; }
static uint32_t ota_now_ms(void) { return now_ms; }
static void ota_idle(void) { now_ms++; }
static int ota_erase(uint32_t off) { (void)off; return -1; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static int ota_fread(uint32_t off, void *p, uint32_t n) { memcpy(p, nor + off, n); return 0; }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; }
#include "../bsp/chip/ota.c"

static int ldr_fread(uint32_t off, void *p, uint32_t n) { memcpy(p, nor + off, n); return 0; }
static int ldr_erase(uint32_t off)
{
    if (off < 0x4000u || off >= 0xFC000u || (off & 0xFFFu)) bad_range++;
    if (off < 0x4000u) { printf("ERASE IN THE HEAD %#x\n", off); exit(1); }
    erases++;
    memset(nor + off, 0xFF, 0x1000);
    return 0;
}
static int ldr_prog(uint32_t off, const void *p, uint32_t n)
{
    const uint8_t *s = p;
    uint32_t i;
    if (off < 0x4000u || off + n > FL_APP_END || ((off & 0xFFu) + n) > 256u) bad_range++;
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
static void ldr_record_clear(void) { record_cleared++; }
static void ldr_progress(uint32_t done, uint32_t total) { (void)done; (void)total; }
static void ldr_res_window(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; }   /* (no resource region in these packages) */
#include "../bsp/loader/ldr_core.c"

static uint8_t *load_logical(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *raw = malloc(0x200000), *lg;
    size_t n, i;
    if (!f) { perror(path); exit(2); }
    n = fread(raw, 1, 0x200000, f);
    fclose(f);
    lg = malloc(n);
    for (i = 0; i < 20; i++) memcpy(lg + i * 47, raw + i * 48, 47);
    memcpy(lg + 20 * 47, raw + 20 * 48, n - 20 * 48);
    *len = n - 20;
    free(raw);
    return lg;
}

static uint32_t flash_off(const uint8_t *lg)        /* flash.bin offset in the logical image */
{
    uint8_t h[0x400];
    uint32_t i, n;
    memcpy(h, lg, sizeof h);
    ota_jl_enc(h, 0x40);
    n = ota_rd16(h + 8);
    for (i = 0; i < n; i++) {
        uint8_t *e = h + 0x40 + i * 0x50u;
        ota_jl_enc(e, 0x50);
        if (ota_rd16(e) == 0) return ota_rd32(e + 8);
    }
    return 0;
}

static int check(const char *what, int ok) { printf("%-64s %s\n", what, ok ? "ok" : "FAIL"); return ok ? 0 : 1; }
static int all(uint32_t lo, uint32_t hi, uint8_t v)  /* nor[lo, hi) all v */
{
    for (; lo < hi; lo++)
        if (nor[lo] != v) return 0;
    return 1;
}

#ifdef LDR_STEPS
/* --steps: an app with steps (fm1.toml app_steps; this test's build: an app's steps, N 0x93000, S0 0xB5000,
 * the release's L1 0xCE000, and the classic build's L1 0xD0000 an FM-1 may have) against the switch matrix written
 * out, not computed: for this build's step, what an install over each kind of installed app erases first */
typedef struct { const char *what; uint32_t end, lo[3], hi[3]; } sw_case_t;   /* by the new step: N, S0, L1 (0xCE000) */
static const sw_case_t SW[] = {
    {"after N (0x93000)", 0x93000u, {0, 0xB5000u, 0xCE000u}, {0, 0xD8000u, 0xDA000u}},
    {"after S0 (0xB5000)", 0xB5000u, {0x93000u, 0, 0xCE000u}, {0xD8000u, 0, 0xDA000u}},
    {"after L1 (0xCE000)", 0xCE000u, {0x93000u, 0xB5000u, 0}, {0xDA000u, 0xDA000u, 0}},
    {"after the classic L1 (0xD0000: the same step)", 0xD0000u, {0x93000u, 0xB5000u, 0xCE000u}, {0xDA000u, 0xDA000u, 0xD0000u}},
    {"after no head of that format", 0, {0, 0xB5000u, 0xCE000u}, {0, 0xD8000u, 0xDA000u}},
    {"after a head ending below normal's (0x5000)", 0x5000u, {0, 0xB5000u, 0xCE000u}, {0, 0xD8000u, 0xDA000u}},
    {"after an end no step has (0xC0000)", 0xC0000u, {0x93000u, 0xB5000u, 0xCE000u}, {0xDA000u, 0xDA000u, 0xDA000u}},
};

static int steps_test(void)
{
    static const uint32_t KEY = 0x980Fu;
    const int col = FL_APP_END == 0x93000u ? 0 : FL_APP_END == 0xB5000u ? 1 : FL_APP_END == 0xCE000u ? 2 : -1;
    uint8_t buf[32];
    uint32_t k;
    int bad = 0, rc, pass;
    bad += check("this build's app end is one of the matrix's steps", col >= 0);
    if (col < 0)
        return bad;
    for (k = 0; k < sizeof SW / sizeof SW[0]; k++)
        for (pass = 0; pass < 2; pass++) {               /* the second: resumed, the old head still there */
            const uint32_t lo = SW[k].lo[col], hi = SW[k].hi[col];
            char what[128];
            memset(nor, 0xFF, sizeof nor);
            memset(nor + 0x5000, 0xA5, 0xE0000u - 0x5000u);
            if (SW[k].end) {                             /* the installed app area's head, its block ending in the slot's
                                                          * last sector (fm1pkg_make.py) */
                uint8_t e[32];
                uint32_t blk = SW[k].end - 0x4000u - 0xD70u, c;
                memset(e, 0xFF, sizeof e);
                e[4] = 0x20; e[5] = 0x01; e[6] = 0x00; e[7] = 0x02;
                e[8] = (uint8_t)blk; e[9] = (uint8_t)(blk >> 8); e[10] = (uint8_t)(blk >> 16); e[11] = (uint8_t)(blk >> 24);
                c = ota_crc16(e + 2, 30, 0);
                e[0] = (uint8_t)c; e[1] = (uint8_t)(c >> 8);
                ldr_sfc(e, 32, 0, KEY);
                memcpy(nor + 0x4000, e, sizeof e);
            }
            if (pass)
                memset(nor + lo, 0xFF, hi - lo);
            erases = 0; bad_range = 0;
            rc = ldr_slot_switch(KEY, buf);
            snprintf(what, sizeof what, "app end %#x, %s%s: [%#x, %#x)", (unsigned)FL_APP_END, SW[k].what,
                     pass ? ", resumed" : "", (unsigned)lo, (unsigned)hi);
            bad += check(what, rc == 0 && erases == (hi - lo) / 0x1000u && all(0x5000u, lo ? lo : 0x5000u, 0xA5) &&
                         all(lo, hi, 0xFF) && all(hi > lo ? hi : 0x5000u, 0xE0000u, 0xA5) && bad_range == 0);
        }
    return bad;
}

/* --range END...: for any app's steps (its own LDR_STEPS), what this
 * build's loader erases after an installed app ending at each END (0: no head): "END LO HI" per line, [LO, HI) the
 * erased sectors as read back from the flash (0 0: none), so a host test compares them with the app's own rule */
static int range_test(int n, char **ends)
{
    static const uint32_t KEY = 0x980Fu;
    uint8_t buf[32];
    int i, bad = 0;
    for (i = 0; i < n; i++) {
        uint32_t end = (uint32_t)strtoul(ends[i], NULL, 0), s, lo = 0, hi = 0;
        memset(nor, 0xFF, sizeof nor);
        memset(nor + 0x5000, 0xA5, 0xE0000u - 0x5000u);
        if (end) {
            uint8_t e[32];
            uint32_t blk = end - 0x4000u - 0xD70u, c;
            memset(e, 0xFF, sizeof e);
            e[4] = 0x20; e[5] = 0x01; e[6] = 0x00; e[7] = 0x02;
            e[8] = (uint8_t)blk; e[9] = (uint8_t)(blk >> 8); e[10] = (uint8_t)(blk >> 16); e[11] = (uint8_t)(blk >> 24);
            c = ota_crc16(e + 2, 30, 0);
            e[0] = (uint8_t)c; e[1] = (uint8_t)(c >> 8);
            ldr_sfc(e, 32, 0, KEY);
            memcpy(nor + 0x4000, e, sizeof e);
        }
        erases = 0; bad_range = 0;
        if (ldr_slot_switch(KEY, buf) != 0 || bad_range)
            bad++;
        for (s = 0x5000u; s < 0xE0000u; s += 0x1000u)
            if (all(s, s + 0x1000u, 0xFF)) {
                if (!hi) lo = s;
                hi = s + 0x1000u;
            }
        if (hi && (!all(lo, hi, 0xFF) || erases != (hi - lo) / 0x1000u))
            bad++;                                       /* (not one contiguous range) */
        printf("%#x %#x %#x\n", (unsigned)end, (unsigned)lo, (unsigned)hi);
    }
    return bad;
}
#endif

int main(int argc, char **argv)
{
    size_t olen;
    uint8_t *old, head[0x4000];
    uint32_t ofo, nfo, old_end, top;
    int bad = 0, rc;
#ifdef LDR_STEPS
    if (argc == 2 && !strcmp(argv[1], "--steps")) {
        bad = steps_test();
        printf("ldr_slot_test --steps %s\n", bad ? "FAILED" : "passed");
        return bad ? 1 : 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--range"))
        return range_test(argc - 2, argv + 2) ? 1 : 0;
#endif
    if (argc < 4) { printf("usage: ldr_slot_test OLD.fwsc NEW.fwsc OLD_END\n"); return 2; }
    old = load_logical(argv[1], &olen);
    logical = load_logical(argv[2], &logical_len);
    old_end = (uint32_t)strtoul(argv[3], 0, 0);
    ofo = flash_off(old);
    nfo = flash_off(logical);
    top = old_end > FL_APP_END ? old_end : FL_APP_END;
    memset(nor, 0xFF, sizeof nor);                   /* the device: OLD's flash.bin, data above it (0xA5) */
    memset(nor + old_end, 0xA5, 0xE0000u - old_end);
    memcpy(nor, old + ofo, old_end);
    memcpy(head, nor, sizeof head);
    rc = ldr_session();
    printf("  rc %d, %u requests, %u sector erases (old end %#x, this one %#x)\n", rc, requests, erases, old_end,
           (unsigned)FL_APP_END);
    bad += check("install completes", rc == 0);
    bad += check("app area == the new package's flash.bin",
                 !memcmp(nor + 0x4000, logical + nfo + 0x4000, FL_APP_END - 0x4000));
    bad += check("flash head [0, 0x4000) untouched", !memcmp(nor, head, sizeof head));
    if (old_end > FL_APP_END)
        bad += check("going down: the old app's code above this end erased", all(FL_APP_END, old_end, 0xFF));
#ifdef LDR_CLEAR_END
    if (old_end < FL_APP_END) {
        bad += check("going up: [FL_APP_END, LDR_CLEAR_END) erased", all(FL_APP_END, LDR_CLEAR_END, 0xFF));
        top = LDR_CLEAR_END;
    }
#endif
    bad += check("the data above kept", all(top, 0xE0000u, 0xA5));
    bad += check("no write outside the allowed windows", bad_range == 0);
    erases = 0;
    rc = ldr_session();
    bad += check("the same package again (the same slot now): nothing erased", rc == 0 && erases == 0);
    printf("%s\n", bad ? "SLOT LOADER TEST FAILED" : "slot loader test passed");
    return bad != 0;
}
