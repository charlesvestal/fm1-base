/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Henri David */
/* The Link example (fm1.toml wifi_stack = "own", link = true): the FM-1 joins a Wi-Fi network and a Link session (Live,
 * the iOS apps), shows the session's tempo, beat and peers, and clicks on each beat. GPL-3.0-only, because Ableton Link
 * (link/) is: an app with link = true is GPL-3.0 as a whole (LICENSING.md). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "LINK"
#define APP_SPLASH_RGB RGB(90, 200, 160)
#define APP_BLOCK 256u
#define APP_TICK                                 /* the click's time, checked every millisecond */
void app_tick(void);

/* the store: the panel calibration (the platform's), Bluetooth's address (the radio is Bluetooth's: ble_stack = "own"),
 * and the Wi-Fi networks kept with the mode (plat.h PLAT_WIFI_STORE_SIZE) */
enum { OBJ_PANEL, OBJ_BLE, OBJ_WIFI, OBJ_NOBJ };
#define APP_STORE_MAP {0xDC000u, 0xDE000u, 0xDA000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
