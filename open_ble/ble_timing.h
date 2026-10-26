/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FELUCCA_BLE_TIMING_H
#define FELUCCA_BLE_TIMING_H
#include <stdint.h>
#include "ble_diag.h"
typedef enum {
    BLE_WORK_SERVICE, BLE_WORK_KEY, BLE_WORK_RX, BLE_WORK_CONTROL,
    BLE_WORK_HOUSEKEEP, BLE_WORK_DELIVERY, BLE_WORK_RESOLUTION,
    BLE_WORK_DRIVER, BLE_WORK_COMMAND, BLE_WORK_STOP, BLE_WORK_ADV_PREP,
    BLE_WORK_ENTROPY_SAMPLE, BLE_WORK_ENTROPY_RESEED, BLE_WORK_ENTROPY_OUTPUT,
    BLE_WORK_ENTROPY_REKEY, BLE_WORK_CONTROL_RX, BLE_WORK_REFILL,
    BLE_WORK_RESET_CLEAR, BLE_WORK_RESET_DEFAULTS, BLE_WORK_RESET_STATS,
    BLE_WORK_RESET_COMPLETE,
#if FM1B_BLE_RELIABILITY
    BLE_WORK_RELIABILITY,
#endif
    BLE_WORK_COUNT
} ble_work_kind_t;
typedef struct {
    uint32_t dispatch_max,dispatch_live_max,dispatch_negative,wall_max,own_max,item_max,item_over_100us;
    uint32_t own_over_250us,pass_over_budget,refill_max,refill_ccm_max,refill_other_max,refill_over_80us;
#if FM1B_BLE_DIAG
    uint32_t kind_max[BLE_WORK_COUNT],kind_count[BLE_WORK_COUNT],kind_over_100us[BLE_WORK_COUNT];
    uint16_t item_opcode,kind_opcode[BLE_WORK_COUNT];
#else
    uint16_t item_opcode;
#endif
    uint8_t item_kind;
} ble_timing_diag_t;
void ble_time_preempt_enter(void);
void ble_time_preempt_leave(void);
uint32_t ble_time_own(void);
void ble_time_dispatch(uint32_t request,uint32_t entry);
void ble_time_pass_begin(uint32_t wall);
void ble_time_pass_end(void);
int ble_time_budget(void);
#if FM1B_BLE_RELIABILITY
int ble_time_has_room(uint32_t ticks);
#endif
uint32_t ble_time_item_begin(void);
void ble_time_item_end(uint32_t start,ble_work_kind_t kind,uint16_t opcode);
const char *ble_time_kind_name(unsigned kind);
void ble_time_ccm_add(uint32_t ticks);
uint32_t ble_time_ccm_total(void);
void ble_time_refill_end(uint32_t start,uint32_t ccm_before);
void ble_time_get_diag(ble_timing_diag_t *out);
void ble_time_reset(void);
#endif
