/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Henri David */
/* An update begins (the upgrade command F0 22 24 35 7F F7, or the soft key's UBOOT F0 22 24 35 7D F7): the app stops
 * cleanly first (APP_UPDATE_BEGIN, device/main_fm1.c update_begin), before anything is staged or erased and before
 * the restart.
 *   1. bsp/chip/ota.c on a simulated NOR: the handshake (cmd 0x11) that every main-loop pass answers touches no flash;
 *      the main loop's update step in main_fm1.c's order (update_begin, app_panic, ota_session) has the app's stop
 *      before the first erase and the first write (a host that stops answering: the session gives up, unstages)
 *   2. the time cap (device/update_begin.h): the update goes ahead after PLAT_UPDATE_BEGIN_MS whatever the hook says
 *      (one that never says it is done, one that is done at once, none), and nothing is erased before it
 *   3. device/main_fm1.c's text: the app loop's update and soft-key steps wait for update_ready first; update_ready asks
 *      app_update_begin only for an app that defines APP_UPDATE_BEGIN (fm1-base's own: nothing), through
 *      update_begin_ready, the watchdog fed on both sides; safe mode (no app) does not ask it
 *   cc -o update_begin_test tests/update_begin_test.c && ./update_begin_test */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FELUCCA_ID "FM-1_900"
#define FELUCCA_OTA_DRYRUN 0

static uint8_t nor[0x100000];
static uint8_t rx[64];
static uint32_t rx_len, rx_full, now_ms, ident_replies;
static uint32_t erases, progs, begins, erases_at_begin, progs_at_begin, panics, panic_after_begin;

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

/* ---- ota.c's hooks: a host that asks the identity, then goes silent ---- */
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    if (p[0] != 0xF0 || p[n - 1] != 0xF7) { printf("bad wire framing\n"); exit(1); }
    ident_replies++;                                 /* (the identity, or a read request the host ignores) */
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n)
{
    if (!rx_full) return 0;
    *p = rx; *n = rx_len;
    return 1;
}
static void ota_frame_done(void) { rx_full = 0; }
static uint32_t ota_now_ms(void) { return now_ms; }
static void ota_idle(void) { now_ms += 10; }
static int ota_erase(uint32_t off) { erases++; memset(nor + off, 0xFF, 4096); return 0; }
static int ota_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    progs++;
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
static int ota_fread(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static void ota_show(uint32_t step, int32_t code) { (void)step, (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; printf("committed without a host\n"); exit(1); }

#include "../bsp/chip/ota.c"
#include "../device/update_begin.h"

/* ---- the app and the platform's step, as main_fm1.c has them ---- */
static int app_update_begin(void) { begins++; erases_at_begin = erases; progs_at_begin = progs; return 0; }
static uint32_t hook_calls, hook_left;
static int hook_never_done(void) { hook_calls++; return 1; }          /* a hook that hangs: always still stopping */
static int hook_done_after(void) { hook_calls++; return hook_left && hook_left--; }
static int hook_done_at_once(void) { hook_calls++; return 0; }
static void app_panic(void) { panics++; panic_after_begin = begins != 0; }

static void handshake(void)                         /* F0 pack7(00 59 11 000000 chk) F7, as M-UPGRADE asks */
{
    static const uint8_t m[7] = {0x00, 0x59, 0x11, 0, 0, 0, 0xFF};
    rx_len = pack7(m, sizeof m, rx);
    rx_full = 1;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b = malloc(1 << 21);
    size_t n;
    if (!f || !b) { printf("cannot read %s\n", path); exit(1); }
    n = fread(b, 1, (1 << 21) - 1, f);
    b[n] = 0;
    fclose(f);
    return b;
}
static const char *after(const char *s, const char *a)    /* the last a in s */
{
    const char *p = strstr(s, a), *q = p;
    while (q && (q = strstr(q + 1, a)))
        p = q;
    return p;
}
static int in_order(const char *from, const char *a, const char *b, const char *c)
{
    const char *pa = from ? strstr(from, a) : 0, *pb = pa ? strstr(pa, b) : 0, *pc = pb && c ? strstr(pb, c) : pb;
    return pa && pb && pc;
}

int main(void)
{
    char *m;
    const char *loop, *safe, *ub;
    int i;

    for (i = 0; i < 3; i++) {                        /* every main-loop pass: the handshake answered, no flash touched */
        handshake();
        assert(ota_service() == 7u && ota_dec[2] == 0x11);
    }
    assert(ident_replies == 3 && erases == 0 && progs == 0);
    printf("handshake (cmd 0x11) x3: answered, nothing erased or written     ok\n");

    /* the main loop's update step, in main_fm1.c's order (checked below): the host asked for the update, then stops
     * answering: the session gives up and unstages (its erases come after the app's stop) */
    ota_try_ms = 50u;
    app_update_begin();
    app_panic();
    ota_session();
    assert(begins == 1 && erases_at_begin == 0 && progs_at_begin == 0 && panics == 1 && panic_after_begin);
    assert(erases > 0);
    printf("update step: the app's stop before the first erase (%u erases after) ok\n", (unsigned)erases);

    {
        /* the cap: a hook that is never done, then the update goes ahead exactly PLAT_UPDATE_BEGIN_MS after the first ask,
         * and nothing was erased or written meanwhile (the main loop's passes run on, one a millisecond here) */
        update_begin_t st = {0};
        uint32_t t = 1000u, e0 = erases, p0 = progs;
        hook_calls = 0;
        while (!update_begin_ready(&st, t, hook_never_done)) {
            assert(erases == e0 && progs == p0);
            t++;
            assert(t - 1000u <= PLAT_UPDATE_BEGIN_MS);
        }
        assert(t - 1000u == PLAT_UPDATE_BEGIN_MS && hook_calls == PLAT_UPDATE_BEGIN_MS);
        assert(PLAT_UPDATE_BEGIN_MS >= 100u && PLAT_UPDATE_BEGIN_MS <= 1000u);
        printf("a hook never done: the update goes ahead after %u ms           ok\n", (unsigned)PLAT_UPDATE_BEGIN_MS);

        /* the next update asks afresh, a hook done after three asks lets it go ahead at the third */
        hook_calls = 0;
        hook_left = 2;
        for (t = 5000u; !update_begin_ready(&st, t, hook_done_after); t++)
            assert(t < 5010u);
        assert(t == 5002u && hook_calls == 3);
        hook_calls = 0;
        assert(update_begin_ready(&st, 9000u, hook_done_at_once) && hook_calls == 1);
        assert(update_begin_ready(&st, 9001u, 0));            /* an app without the hook */
        printf("a hook done after 3 asks, at once, none: no wait beyond it        ok\n");

        /* the millisecond counter wrapping in the middle of the wait, and the first ask at time 0 */
        for (t = 0xFFFFFF00u, hook_calls = 0; !update_begin_ready(&st, t, hook_never_done); t++)
            assert(hook_calls <= PLAT_UPDATE_BEGIN_MS);
        assert(hook_calls == PLAT_UPDATE_BEGIN_MS);
        for (t = 0, hook_calls = 0; !update_begin_ready(&st, t, hook_never_done); t++)
            assert(hook_calls <= PLAT_UPDATE_BEGIN_MS);
        assert(hook_calls == PLAT_UPDATE_BEGIN_MS);
        printf("the cap across the millisecond counter's wrap and at time 0       ok\n");

        /* the whole update step with the hook that hangs: the app's stop never ends, the session still runs, after the cap */
        ota_try_ms = 50u;
        st.on = 0;
        erases_at_begin = progs_at_begin = 0;
        e0 = erases;
        for (t = 20000u; !update_begin_ready(&st, t, hook_never_done); t++)
            assert(erases == e0);
        app_panic();
        ota_session();
        assert(erases > e0);
        printf("a hook that hangs: the update session still runs (erases %u)      ok\n", (unsigned)(erases - e0));
    }

    m = slurp("device/main_fm1.c");
    loop = after(m, "if (usb.ota_req) {");              /* the app's main loop (the last one; the first is safe mode's) */
    safe = strstr(m, "static void safe_main(void)");
    assert(loop && safe && loop > safe);
    assert(in_order(loop, "if (usb.ota_req && update_ready()) {", "usb.ota_req = 0;", "app_panic();"));
    assert(in_order(loop, "update_ready()", "app_panic();", "ota_session();"));
    printf("main_fm1.c: update: update_ready, app_panic, then ota_session      ok\n");
    ub = strstr(loop, "if (usb.uboot_req && update_ready())");
    assert(ub && in_order(ub, "update_ready()", "enter_uboot(\"UBOOT (USB)\");", 0));
    assert(!strstr(ub, "app_panic();") || strstr(ub, "app_panic();") > strstr(ub, "enter_uboot("));
    printf("main_fm1.c: soft key: update_ready before enter_uboot (no app_panic) ok\n");
    /* an app's SysEx stream still open when the update or UBOOT starts (begun under PLAT_SYSEX_HOLD_MS ago, so
     * plat_sysex_hold has not let it go yet): ended first, else the ring stays held and the update's replies never
     * leave */
    assert(in_order(strstr(m, "static void update_sysex_end(void)"), "if (sxs.on)", "(void)plat_sysex_end();", 0));
    assert(in_order(loop, "app_panic();", "update_sysex_end();", "fm1_clock_restore();"));
    assert(in_order(strstr(m, "static void enter_uboot("), "update_sysex_end();", "fm1_clock_restore();", 0));
    printf("main_fm1.c: an open SysEx stream ended before the update and UBOOT ok\n");
    {
        const char *s = strstr(safe, "if (usb.ota_req) {"), *e = strstr(safe, "static void splash(void)");
        const char *f = strstr(m, "static int update_ready(void)");
        assert(s && e && s < e && !(strstr(safe, "update_ready()") && strstr(safe, "update_ready()") < e));
        assert(f && f < safe);
        assert(in_order(f, "#ifdef APP_UPDATE_BEGIN", "fm1_wdt_feed();\n    r = update_begin_ready(&st, fm1_ms, app_update_begin);\n    fm1_wdt_feed();", "#else"));
        assert(in_order(f, "#else", "return 1;", "#endif\n}"));   /* (no hook: nothing kept, nothing waited for) */
        assert(strstr(f, "#endif\n}") < strstr(f, "static void enter_uboot"));
        assert(strstr(m, "#include \"update_begin.h\""));
    }
    printf("main_fm1.c: only with APP_UPDATE_BEGIN, the watchdog fed; not in safe mode ok\n");

    assert(!strstr(slurp("template/app.h"), "APP_UPDATE_BEGIN"));
    printf("the template: none     ok\n");
    printf("update_begin tests passed\n");
    return 0;
}
