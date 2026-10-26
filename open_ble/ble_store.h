/* SPDX-License-Identifier: MIT OR MPL-2.0 
 * Copyright (c) 2026 Julius Vitkauskas */
#pragma once
#include <stdint.h>
#define BLE_BOND_CAPACITY 8u
#define BLE_BOND_PEER_SIZE 144u
#define BLE_BOND_BYTES (BLE_BOND_CAPACITY*BLE_BOND_PEER_SIZE)
#define BLE_OBJECT_SIZE (80u+BLE_BOND_BYTES+4u)
#define BLE_LEGACY_OBJECT_SIZE 1108u
#define BLE_BOND_SLOTS 8u /* old FLBD records, migration only */
/* Platform supplies fm1-base's A/B OBJ_BLE object. Main context only. */
int ble_port_object_load(void *dst, uint32_t n);
int ble_port_object_save(const void *src, uint32_t n);
void ble_object_init(void);
int ble_object_seed_load(uint8_t record[64]);
int ble_object_seed_save(const uint8_t record[64], uint32_t *ticks);
void ble_object_bonds_load(uint8_t *peers,uint32_t *replacements);
int ble_object_bonds_save(const uint8_t *peers,uint32_t replacements);
