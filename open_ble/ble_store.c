/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
/* Atomic A/B OBJ_BLE snapshot. The verified baseline advances only after
 * readback. v1 is translated in RAM; only an ordinary seed/bond save migrates
 * flash. The normal boot seed advance still happens, exactly as in R4. */
#include "ble_memory.h"
#include "ble_crypto.h"
#include "ble_seed.h"
#include "ble_s1.h"
#include "ble_store.h"
#include "ble_bonds.h"
static uint8_t object[BLE_OBJECT_SIZE] __attribute__((section(".pool.ble")));
static int object_valid(const uint8_t *p,unsigned n)
{
    if(memcmp(p,"FMOB",4) || p[7] || ble_get32(p+12) ||
       ble_get32(p+n-4)!=ble_crc32(p,n-4)) return 0;
    if(n==BLE_LEGACY_OBJECT_SIZE)
        return p[4]==1 && !p[5] && !p[6] && !ble_get32(p+8);
    return n==BLE_OBJECT_SIZE && p[4]==2 && p[5]==BLE_BOND_CAPACITY && p[6]==BLE_BOND_PEER_SIZE;
}
static void object_empty(void)
{
    memset(object,0,sizeof object); memcpy(object,"FMOB",4);
    object[4]=2; object[5]=BLE_BOND_CAPACITY; object[6]=BLE_BOND_PEER_SIZE;
    memset(object+16,255,64);
}
void ble_object_init(void)
{
    int n=ble_port_object_load(object,sizeof object);
    if(n==(int)BLE_LEGACY_OBJECT_SIZE && object_valid(object,(unsigned)n)) {
        uint8_t old[BLE_LEGACY_OBJECT_SIZE];
        memcpy(old,object,sizeof old); object_empty(); memcpy(object+16,old+16,64);
        ble_bonds_migrate(old+80,object+80); ble_secure_zero(old,sizeof old);
    } else if(n!=(int)sizeof object || !object_valid(object,sizeof object)) object_empty();
    ble_put32(object+BLE_OBJECT_SIZE-4,ble_crc32(object,BLE_OBJECT_SIZE-4));
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
       !object_valid(verify,sizeof verify) || memcmp(next,verify,sizeof verify)) return BLE_FLASH_LATE_READY;
    memcpy(object,next,sizeof object); return BLE_FLASH_OK;
}
int ble_object_seed_load(uint8_t record[64]) { memcpy(record,object+16,64); return 0; }
int ble_object_seed_save(const uint8_t record[64],uint32_t *ticks)
{
    uint8_t next[BLE_OBJECT_SIZE]; uint32_t start=ble_port_ticks();
    memcpy(next,object,sizeof next); memcpy(next+16,record,64);
    int rc=object_commit(next); *ticks=ble_port_ticks()-start; return rc;
}
void ble_object_bonds_load(uint8_t *peers,uint32_t *replacements)
{
    memcpy(peers,object+80,BLE_BOND_BYTES); *replacements=ble_get32(object+8);
}
int ble_object_bonds_save(const uint8_t *peers,uint32_t replacements)
{
    uint8_t next[BLE_OBJECT_SIZE];
    memcpy(next,object,sizeof next); memcpy(next+80,peers,BLE_BOND_BYTES); ble_put32(next+8,replacements);
    return object_commit(next);
}
