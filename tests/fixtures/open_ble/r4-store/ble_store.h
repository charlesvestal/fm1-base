/* SPDX-License-Identifier: MIT OR MPL-2.0 
 * Copyright (c) 2026 Julius Vitkauskas */
#pragma once
#include <stdint.h>
#define BLE_OBJECT_SIZE 1108u
#define BLE_BOND_SLOTS 8u
/* Platform supplies fm1-base's A/B OBJ_BLE object. Main context only. */
int ble_port_object_load(void *dst, uint32_t n);
int ble_port_object_save(const void *src, uint32_t n);
void ble_object_init(void);
int ble_object_seed_load(uint8_t record[64]);
int ble_object_seed_save(const uint8_t record[64], uint32_t *ticks);
int ble_object_bond_load(unsigned slot, uint8_t record[128]);
int ble_object_bonds_save(const uint8_t *records);
