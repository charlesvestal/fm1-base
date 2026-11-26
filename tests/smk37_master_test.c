/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* The SMK-37's virtual MASTER (device/smk37_master.h): K5 turned for a while saves the level once, SMK_MASTER_SAVE_MS
 * after the last detent, whatever millisecond a detent lands on (a turn on an even millisecond saved at the next pass:
 * a store write a detent, the key scan and the sound held up while turning); plat_master_set the same; a level
 * already saved is not written again; the level stays within 0..4096.
 *   cc -std=c99 tests/smk37_master_test.c */
#include <stdint.h>
#include <stdio.h>

static uint32_t fm1_ms;
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static struct { uint32_t master; } panel;
static struct { int32_t master; } remote = {-1};
static volatile uint32_t master_pos;
static uint32_t mirror_in_master(uint32_t pot) { return pot; }
enum { OBJ_PANEL = 3 };
static int saves;
static int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    (void)src, (void)len;
    saves += obj == OBJ_PANEL;
    return 0;
}
#include "../device/smk37_master.h"

static int bad;
static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    bad += !ok;
}
static void passes(uint32_t ms, int32_t steps_at_first)  /* the main loop for ms milliseconds, a few passes each */
{
    uint32_t t, k;
    for (t = 0; t < ms; t++, fm1_ms++)
        for (k = 0; k < 4u; k++)
            smk_master_poll(t == 0 && k == 0 ? steps_at_first : 0);
}

int main(void)
{
    uint32_t i;
    printf("smk37 master\n");
    fm1_ms = 1000;
    panel.master = 2896;
    smk_master_load();
    check("the saved level at start", master_pos == 2896u);
    for (i = 0; i < 100u; i++)                    /* K5 turned: a detent every 17 ms, on even and odd milliseconds */
        passes(17, 1);
    check("no save while turning", saves == 0);
    check("the level follows the detents, up to 4096", master_pos == 4096u);
    passes(SMK_MASTER_SAVE_MS - 17u, 0);          /* (the last detent was 17 ms ago) */
    check("no save before the quiet time", saves == 0);
    passes(2, 0);
    check("one save after the last detent", saves == 1 && panel.master == 4096u);
    passes(10000, 0);
    check("not again", saves == 1);
    fm1_ms = 2000;                                /* an even millisecond, polled at once */
    passes(1, -1);
    check("a detent on an even millisecond: no save at once", saves == 1 && master_pos == 4096u - SMK_MASTER_STEP);
    passes(SMK_MASTER_SAVE_MS + 1u, 0);
    check("saved after the quiet time", saves == 2 && panel.master == 4096u - SMK_MASTER_STEP);
    fm1_ms = 4000;
    plat_master_set(1000);
    smk_master_poll(0);
    check("plat_master_set: shown, not saved at once", master_pos == 1000u && saves == 2);
    passes(SMK_MASTER_SAVE_MS + 1u, 0);
    check("plat_master_set: saved after the quiet time", saves == 3 && panel.master == 1000u);
    passes(5, 1);                                 /* one detent up and one down: the saved level again */
    passes(5, -1);
    passes(SMK_MASTER_SAVE_MS + 1u, 0);
    check("back to the saved level: no write", saves == 3 && master_pos == 1000u);
    passes(1, -100);
    check("the level stops at 0", master_pos == 0u);
    printf("smk37 master: %s\n", bad ? "FAILED" : "all passed");
    return bad != 0;
}
