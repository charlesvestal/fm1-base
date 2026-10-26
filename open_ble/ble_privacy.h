/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FELUCCA_BLE_PRIVACY_H
#define FELUCCA_BLE_PRIVACY_H
#include "ble_link.h"
#define BLE_RL_CAPACITY 10u
#define BLE_RL_BLOCKS_PER_ITEM 1u
typedef struct {
    uint32_t attempts,matches,unresolved,aes_failures,network_rejected;
    uint8_t count,enabled,capacity;
} ble_privacy_diag_t;
void ble_privacy_reset(void);
/* Reset clearing, like LL queues, yields after at most 128 bytes. */
int ble_privacy_reset_step(unsigned offset);
/* Caller excludes the CONNECT_IND interrupt through check and mutation. */
uint8_t ble_privacy_command(uint16_t op,const uint8_t *p,int advertising);
int ble_privacy_accept(const ble_conn_t *c);
/* One list entry / AES block per executor item. 0: more work, 1: done. */
int ble_privacy_resolve_step(void);
void ble_privacy_get_diag(ble_privacy_diag_t *out);
#endif
