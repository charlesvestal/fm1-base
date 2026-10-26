/* SPDX-License-Identifier: MIT OR MPL-2.0 
 * Copyright (c) 2026 Julius Vitkauskas */
/* One atomic A/B snapshot: seed and identity-keyed bond records share OBJ_BLE.
 * No absolute flash addresses. A failed commit never advances the RAM baseline. */
#include "ble_memory.h"
#include "ble_seed.h"
#include "ble_s1.h"
#include "ble_store.h"
static uint8_t object[BLE_OBJECT_SIZE] __attribute__((section(".pool.ble")));
static int object_valid(const uint8_t *p)
{
    return !memcmp(p,"FMOB",4) && p[4]==1 && !p[5] && !p[6] && !p[7] &&
        !ble_get32(p+8) && !ble_get32(p+12) &&
        ble_get32(p+BLE_OBJECT_SIZE-4)==ble_crc32(p,BLE_OBJECT_SIZE-4);
}
void ble_object_init(void)
{
    if(ble_port_object_load(object,sizeof object)!=(int)sizeof object || !object_valid(object)) {
        memset(object,255,sizeof object);
        memcpy(object,"FMOB",4); memset(object+4,0,12); object[4]=1;
    }
}
static int object_commit(uint8_t *next)
{
    uint8_t verify[BLE_OBJECT_SIZE];
    if(!ble_port_storage_idle()) return BLE_FLASH_REFUSED;
    ble_put32(next+BLE_OBJECT_SIZE-4,ble_crc32(next,BLE_OBJECT_SIZE-4));
    int rc=ble_port_object_save(next,BLE_OBJECT_SIZE);
    if(rc) return rc==-8 || rc==-1 ? BLE_FLASH_REFUSED :
        rc==-2 || rc==-3 ? BLE_FLASH_NOT_ISSUED : BLE_FLASH_LATE_READY;
    if(ble_port_object_load(verify,sizeof verify)!=(int)sizeof verify ||
       !object_valid(verify) || memcmp(next,verify,sizeof verify)) return BLE_FLASH_LATE_READY;
    memcpy(object,next,sizeof object);
    return BLE_FLASH_OK;
}
int ble_object_seed_load(uint8_t record[64]) { memcpy(record,object+16,64); return 0; }
int ble_object_seed_save(const uint8_t record[64],uint32_t *ticks)
{
    uint8_t next[BLE_OBJECT_SIZE]; uint32_t start=ble_port_ticks();
    memcpy(next,object,sizeof next); memcpy(next+16,record,64);
    int rc=object_commit(next); *ticks=ble_port_ticks()-start; return rc;
}
int ble_object_bond_load(unsigned slot,uint8_t record[128])
{
    if(slot>=BLE_BOND_SLOTS) return -1;
    memcpy(record,object+80+slot*128u,128); return 0;
}
int ble_object_bonds_save(const uint8_t *records)
{
    uint8_t next[BLE_OBJECT_SIZE];
    memcpy(next,object,sizeof next); memcpy(next+80,records,BLE_BOND_SLOTS*128u);
    return object_commit(next);
}
