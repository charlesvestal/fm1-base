/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Henri David */
/* The real-time MIDI test app (tests/run_tests.sh): an app built with APP_TICK_FAST whose app_tick_fast sends a MIDI
 * clock tick through plat_midi_rt every tenth call (1 kHz), as a CLOCK OUT does; the script checks what the app counted
 * (expect app_fast_ticks, app_rt_sent) beside the simulator's own (expect clocks_out). No sound, a black screen. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "RTTEST"
#define APP_SPLASH_RGB RGB(120, 160, 240)
#define APP_BLOCK 256u
#define APP_TICK_FAST
#define APP_HOST_EXPECT

enum { OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xDC000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_tick_fast(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
int app_expect(const char *what, int *got);
