/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
#pragma once
#ifndef FM1B_BLE_DIAG
#define FM1B_BLE_DIAG 1
#endif
#if FM1B_BLE_DIAG
#define BLE_DIAG_ONLY(...) do { __VA_ARGS__; } while(0)
#else
#define BLE_DIAG_ONLY(...) do {} while(0)
#endif

/* This profile is deliberately absent from Felucca and from release builds. */
#ifndef FM1B_BLE_RELIABILITY
#if FM1B_BLE_OPEN && FM1B_BLE_DIAG && FELUCCA_BLE_STAGE >= 3
#define FM1B_BLE_RELIABILITY 1
#else
#define FM1B_BLE_RELIABILITY 0
#endif
#endif
#if FM1B_BLE_RELIABILITY && !FM1B_BLE_DIAG
#error R7 reliability diagnostics require FM1B_BLE_DIAG
#endif
