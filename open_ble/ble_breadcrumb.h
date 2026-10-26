/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FELUCCA_BLE_BREADCRUMB_H
#define FELUCCA_BLE_BREADCRUMB_H
#include <stdint.h>

#define BLE_BREADCRUMB_MAGIC 0x424c4501u
enum {
    BLE_STAGE_PREPARE=1, BLE_STAGE_READY=2,
    BLE_STAGE_RX=0x10, BLE_STAGE_CONNECT=0x11, BLE_STAGE_ANCHOR=0x12,
    BLE_STAGE_WINDOW=0x13, BLE_STAGE_CREATE=0x14, BLE_STAGE_CONNECTED=0x15,
    BLE_STAGE_EVENT=0x20, BLE_STAGE_WORKER=0x30, BLE_STAGE_HOST=0x40
};
typedef struct {
    uint32_t magic,last,last_ticks,fault,fault_ticks,fault_count;
} ble_breadcrumb_t;

static inline void ble_breadcrumb_init(volatile ble_breadcrumb_t *b)
{
    if(b->magic!=BLE_BREADCRUMB_MAGIC) {
        b->last=b->last_ticks=b->fault=b->fault_ticks=b->fault_count=0;
        b->magic=BLE_BREADCRUMB_MAGIC;
    }
}
/* The stage/state/detail word is one aligned store, including across nested
 * IRQs. Its timestamp is approximate if a higher-priority IRQ intervenes. */
static inline void ble_breadcrumb_mark(volatile ble_breadcrumb_t *b,
    uint8_t stage,uint8_t state,uint16_t detail,uint32_t ticks)
{
    b->last_ticks=ticks;
    b->last=((uint32_t)stage<<24)|((uint32_t)state<<16)|detail;
}
static inline void ble_breadcrumb_fault(volatile ble_breadcrumb_t *b,uint32_t count)
{
    if(b->magic==BLE_BREADCRUMB_MAGIC) {
        b->fault=b->last; b->fault_ticks=b->last_ticks; b->fault_count=count;
    }
}
#endif
