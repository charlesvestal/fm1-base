/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
#pragma once
#include "ble_diag.h"
#include <stdint.h>
enum { BLE_HEALTH_OFF, BLE_HEALTH_STARTING, BLE_HEALTH_ADVERTISING, BLE_HEALTH_LINK };
enum { BLE_GAP_BOOT=1, BLE_GAP_RADIO_START=2, BLE_GAP_RADIO_STOP=4, BLE_GAP_FLASH=8 };
typedef struct {
    uint32_t ms,ticks,epoch;
    uint8_t state,end_state,reasons;
} ble_gap_event_t;
typedef struct { uint32_t session,ms,seen,accepted; } ble_adv_session_t;
typedef struct {
    ble_gap_event_t maximum,last[4];
    uint32_t calls,over,last_ticks;
    uint32_t clock_ticks,clock_ms,fraction;
    uint8_t count,next,active,since,state,phase,initialized,depth[4];
    ble_adv_session_t adv;
#if FM1B_BLE_DIAG
    ble_adv_session_t sessions[4];
#endif
} ble_health_t;
/* All callers hold the port's IRQ lock. Keep this recorder independent of
 * HCI Reset and its counter reset; timestamps extend continuous TIMER4, so
 * missed TIMER5 ticks during flash cannot hide the elapsed wall time. */
extern ble_health_t ble_health;
void ble_health_init(uint32_t ticks,uint32_t ms);
uint32_t ble_health_ms(uint32_t ticks);
void ble_health_sample(uint32_t ticks,uint8_t state,uint32_t epoch);
void ble_health_begin(uint8_t reason);
void ble_health_end(uint8_t reason);
void ble_health_adv_begin(uint32_t ticks);
void ble_health_connect(int accepted);
