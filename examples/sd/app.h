/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Julius Vitkauskas */
/* The SD card example: fm1-base's SD card (fm1.toml sd = true; plat.h plat_sd_*, FatFs) at its smallest, and a test
 * of a card mod (m-card on the FM-1). It starts the card, shows what it found (size, clock, DMA, errors), lists the
 * card's top folder, runs a speed test (a 1 MiB file written, read back and checked), and shows the card to a
 * computer as a USB drive. */
#pragma once
#include <stdint.h>
#include "plat.h"
#include "fatfs/ff.h"

#define APP_NAME "SD CARD"
#define APP_SPLASH_RGB RGB(120, 200, 120)
#define APP_BLOCK 256u
#define APP_CANVAS_PX (240u * 48u)       /* a strip at a time */
#define APP_LOOP                         /* app_loop: disk mode's polls */

enum { OBJ_PANEL, OBJ_NOBJ };            /* the store: the panel calibration (the platform's) */
#define APP_STORE_MAP {0xDC000u}

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
void app_loop(void);

/* the host simulator's checks (host/plat_host.c "expect"): sd_state (plat_sd_start's last answer), sd_files,
 * sd_test (1 passed, -1 failed) */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
