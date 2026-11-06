/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The SMK-37's wheels and sustain pedal (device/smk37_analog.h) from made-up raw readings, with the main loop starting
 * after the settling window as on the unit (the boot's delays are most of it): the rest is the first look after
 * settling, so a jack resting well above 0 reads the pedal up from the first poll (it read down for the first minute
 * while the rest was only taken inside the window); nothing read inside the window counts; the pedal goes down and up
 * by SMK_PEDAL_STEP; held down past SMK_PEDAL_RELEARN_MS, that reading is the new rest; the pitch wheel reads 0 at its
 * centre and bends both ways.
 *   cc -std=c99 tests/smk37_analog_test.c */
#include <stdint.h>
#include <stdio.h>

static struct {
    uint32_t frames;
    uint16_t wheel[2];
    uint16_t pedal_raw;
} fm1_in;                                         /* the fields of felucca-bsp's fm1_in the header reads */
static uint32_t fm1_ms;
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
enum { PLAT_WHEEL_PITCH, PLAT_WHEEL_MOD };
int plat_wheel(int w);
int plat_pedal(void);
#include "../device/smk37_analog.h"

static int bad;
static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    bad += !ok;
}
static void reset(uint32_t frames)                /* a power-on: the main loop's first poll at this frame count */
{
    smk_bend = smk_mod = smk_pc = smk_plo = smk_phi = smk_mlo = smk_mhi = smk_rest_n = smk_pedal_rest = 0;
    smk_pedal_down = 0;
    smk_pedal_t = 0;
    fm1_in.frames = frames;
    fm1_in.wheel[0] = 512;                        /* the pitch wheel at its centre */
    fm1_in.wheel[1] = 600;                        /* the mod wheel down */
    fm1_ms = 1000;
}
static int polls(int n)                           /* n main-loop polls, a frame and a millisecond each: how many read down */
{
    int down = 0;
    while (n--) {
        smk_analog_poll();
        down += plat_pedal();
        fm1_in.frames++;
        fm1_ms++;
    }
    return down;
}

int main(void)
{
    reset(SMK_SETTLE_FRAMES + 100u);              /* the boot's delays: the window is over before the first poll */
    fm1_in.pedal_raw = 700;                       /* a jack resting well above 0, the pedal up */
    check("the main loop starts after settling: the pedal reads up from the first poll", polls(1) == 0);
    check("  and the rest is that first look (700)", smk_pedal_rest == 700);
    check("  and stays up while the jack rests (2000 polls)", polls(2000) == 0);
    fm1_in.pedal_raw = 700 - SMK_PEDAL_STEP - 50;
    polls(1);
    check("the jack SMK_PEDAL_STEP below its rest: the pedal is down", plat_pedal() == 1);
    fm1_in.pedal_raw = 690;
    polls(1);
    check("back near its rest: up", plat_pedal() == 0);
    fm1_in.pedal_raw = 700 + SMK_PEDAL_STEP + 50;
    polls(1);
    check("SMK_PEDAL_STEP above its rest (the other polarity): down", plat_pedal() == 1);
    fm1_in.pedal_raw = 700;
    polls(1);

    reset(0);                                     /* polled inside the window too: nothing read there counts */
    fm1_in.pedal_raw = 0;
    polls((int)SMK_SETTLE_FRAMES);
    check("inside the settling window: no rest taken, the pedal up", smk_pedal_rest == 0 && plat_pedal() == 0);
    fm1_in.pedal_raw = 650;
    check("the first look after the window is the rest (650): up", polls(1) == 0 && smk_pedal_rest == 650);

    reset(SMK_SETTLE_FRAMES);
    fm1_in.pedal_raw = 100;                       /* no pedal at power-on (a jack reading low) */
    polls(1);
    fm1_in.pedal_raw = 800;                       /* a pedal plugged in afterwards: down, for now */
    polls(1);
    check("a pedal plugged in after power-on reads down at first", plat_pedal() == 1);
    polls((int)SMK_PEDAL_RELEARN_MS + 2);
    check("  held SMK_PEDAL_RELEARN_MS: its reading is the new rest, up", plat_pedal() == 0 && smk_pedal_rest == 800);

    reset(SMK_SETTLE_FRAMES + 10u);
    polls(1);
    check("the pitch wheel at its centre reads 0", plat_wheel(PLAT_WHEEL_PITCH) == 0);
    fm1_in.wheel[0] = 512 + 200;
    polls(1);
    check("  pushed up: a bend up", plat_wheel(PLAT_WHEEL_PITCH) > 0);
    fm1_in.wheel[0] = 512 - 200;
    polls(1);
    check("  pulled down: a bend down", plat_wheel(PLAT_WHEEL_PITCH) < 0);
    fm1_in.wheel[0] = 512;
    polls(1);
    check("  back: 0", plat_wheel(PLAT_WHEEL_PITCH) == 0);
    printf("%s\n", bad ? "SMK37 ANALOG TEST FAILED" : "smk37 analog test passed");
    return bad != 0;
}
