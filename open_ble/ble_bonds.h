/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FELUCCA_BLE_BONDS_H
#define FELUCCA_BLE_BONDS_H
#include <stdint.h>
/* Cache generation, not firmware revision. Bump for a changed attribute schema
 * or a deliberate cache repair. Zero is the pre-R3 (unknown) generation. */
#define BLE_GATT_DB_VERSION 3u
union ble_store_key;
union ble_store_value;
typedef struct {
    uint32_t writes,erases,failures,malformed,private_refused,private_dropped,orphan_dropped,net_noops,replacements;
    uint8_t pending,records,count,capacity;
} ble_bond_diag_t;
typedef struct {
    uint8_t type,addr[6],rank,sc;
    uint16_t cccd_handle[2],cccd_flags[2];
} ble_bond_peer_diag_t;
#if FM1B_BLE_OPEN
void ble_bonds_touch(uint8_t type,const uint8_t identity[6]);
/* Enumerates identities, most recent first (rank 1). No key material. */
int ble_bonds_peer(unsigned index,ble_bond_peer_diag_t *out);
void ble_bonds_replaced(void);
void ble_bonds_migrate(const uint8_t *legacy,uint8_t *peers);
#endif
void ble_bonds_init(void);
void ble_bonds_flush(void);
void ble_bonds_get_diag(ble_bond_diag_t *out);
int ble_bonds_read(int type,const union ble_store_key *key,union ble_store_value *out);
int ble_bonds_write(int type,const union ble_store_value *value);
int ble_bonds_delete(int type,const union ble_store_key *key);
int ble_bonds_db_version(uint8_t type,const uint8_t identity[6],uint32_t *version);
int ble_bonds_db_confirm(uint8_t type,const uint8_t identity[6],uint32_t version);
#endif
