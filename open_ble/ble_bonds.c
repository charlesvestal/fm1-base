/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* Main-context identity cache; no raw NimBLE structures are persisted.
 * FM1B_BLE_OPEN snapshots net state through fm1-base's A/B OBJ_BLE.
 * The fixed-address journal below is retained only for the original native
 * S3/S3.1 regression suites; it is preprocessed out of open firmware. */
#include <string.h>
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "ble_link.h"
#include "ble_seed.h"
#include "ble_bonds.h"
#if FM1B_BLE_OPEN
#include "ble_store.h"
#endif
#if FM1B_BLE_OPEN
#include "ble_bond_capacity.inc"
#else
#define BOND_BASE 0x96000u
#define BOND_COUNT 8
static struct {
    struct { uint8_t type,active,dirty,saved_type,saved_active,p[80],saved[80]; } entry[BOND_COUNT];
    ble_bond_diag_t d; uint32_t sequence; uint8_t next;
} bonds __attribute__((section(".pool.ble")));
static int identity(ble_addr_t *a)
{
    if(a->type>3) return 0;
    a->type&=1u;
    return a->type==0 || ble_random_valid(a->val);
}
static void reconcile(void)
{
    unsigned i; bonds.d.pending=0;
    for(i=0;i<BOND_COUNT;i++) {
        bonds.entry[i].dirty=bonds.entry[i].active!=bonds.entry[i].saved_active ||
            (bonds.entry[i].active && (bonds.entry[i].type!=bonds.entry[i].saved_type ||
                memcmp(bonds.entry[i].p,bonds.entry[i].saved,80)));
        bonds.d.pending|=bonds.entry[i].dirty;
    }
}
static int record_valid(const uint8_t *p)
{
    unsigned i;
    if(memcmp(p,"FLBD",4) || p[4]!=1 || p[5]<1 || p[5]>3 || p[6]>=BOND_COUNT || p[7]>1 ||
        ble_get32(p+124)!=ble_crc32(p,124)) return 0;
    for(i=92;i<124;i++) if(p[i]) return 0;
    if(p[7] && (p[12]>1 || (p[5]<3 && (p[19]<7 || p[19]>16 || p[78]>31)))) return 0;
    return 1;
}
void ble_bonds_init(void)
{
    uint8_t record[128]; unsigned slot,i; int nonblank;
    memset(&bonds,0,sizeof bonds);
    for(slot=0;slot<
#if FM1B_BLE_OPEN
        BLE_BOND_SLOTS
#else
        30u
#endif
        ;slot++) {
        if(
#if FM1B_BLE_OPEN
            ble_object_bond_load(slot,record)
#else
            ble_port_flash_read(BOND_BASE+slot*128u,record,128)
#endif
        ) { bonds.d.failures++; continue; }
        nonblank=0; for(i=0;i<128;i++) if(record[i]!=255) nonblank=1;
        if(!nonblank) continue;
        bonds.next=(uint8_t)(slot+1u);
        if(!record_valid(record)) { bonds.d.malformed++; continue; }
        i=record[6]; bonds.entry[i].type=record[5]; bonds.entry[i].active=record[7];
        memcpy(bonds.entry[i].p,record+12,80); bonds.sequence=ble_get32(record+8);
    }
    for(i=0;i<BOND_COUNT;i++) {
        ble_addr_t a;
        bonds.entry[i].saved_type=bonds.entry[i].type; bonds.entry[i].saved_active=bonds.entry[i].active;
        memcpy(bonds.entry[i].saved,bonds.entry[i].p,80);
        a.type=bonds.entry[i].p[0]; memcpy(a.val,bonds.entry[i].p+1,6);
        if(bonds.entry[i].active && !identity(&a)) { bonds.entry[i].active=0; bonds.d.private_dropped++; }
    }
    reconcile();
}
static int match(int type,const union ble_store_key *key,unsigned i)
{
    const uint8_t *p=bonds.entry[i].p; const ble_addr_t *addr;
    if(!bonds.entry[i].active || bonds.entry[i].type!=type) return 0;
    addr=type==3 ? &key->cccd.peer_addr : &key->sec.peer_addr;
    if(ble_addr_cmp(addr,BLE_ADDR_ANY) && ((addr->type&~2u)!=p[0] || memcmp(addr->val,p+1,6))) return 0;
    return type!=3 || !key->cccd.chr_val_handle || key->cccd.chr_val_handle==ble_get16(p+8);
}
static int find(int type,const union ble_store_key *key)
{
    unsigned i,skip=type==3 ? key->cccd.idx : key->sec.idx;
    for(i=0;i<BOND_COUNT;i++) if(match(type,key,i) && !skip--) return (int)i;
    return -1;
}
int ble_bonds_read(int type,const union ble_store_key *key,union ble_store_value *out)
{
    int i=find(type,key); const uint8_t *p; unsigned j;
    if(i<0) return BLE_HS_ENOENT;
    /* NimBLE's typed wrappers pass a pointer to the selected member alone. */
    memset(out,0,type==3 ? sizeof out->cccd : sizeof out->sec); p=bonds.entry[i].p;
    if(type==3) {
        out->cccd.peer_addr.type=p[0]; memcpy(out->cccd.peer_addr.val,p+1,6);
        out->cccd.chr_val_handle=ble_get16(p+8); out->cccd.flags=ble_get16(p+10); out->cccd.value_changed=p[12]&1u;
    } else {
        out->sec.peer_addr.type=p[0]; memcpy(out->sec.peer_addr.val,p+1,6);
        out->sec.key_size=p[7]; out->sec.ediv=ble_get16(p+8);
        for(j=0;j<8;j++) out->sec.rand_num|=(uint64_t)p[10+j]<<(8u*j);
        memcpy(out->sec.ltk,p+18,16); memcpy(out->sec.irk,p+34,16); memcpy(out->sec.csrk,p+50,16);
        out->sec.ltk_present=p[66]&1u; out->sec.irk_present=(p[66]>>1)&1u;
        out->sec.csrk_present=(p[66]>>2)&1u; out->sec.authenticated=(p[66]>>3)&1u; out->sec.sc=(p[66]>>4)&1u;
    }
    return 0;
}
int ble_bonds_write(int type,const union ble_store_value *value)
{
    uint8_t p[80]={0}; union ble_store_key key; int found; unsigned i,count=0;
    memset(&key,0,sizeof key);
    if(type==3) {
        key.cccd.peer_addr=value->cccd.peer_addr; key.cccd.chr_val_handle=value->cccd.chr_val_handle;
        p[0]=value->cccd.peer_addr.type; memcpy(p+1,value->cccd.peer_addr.val,6);
        ble_put16(p+8,value->cccd.chr_val_handle); ble_put16(p+10,value->cccd.flags); p[12]=!!value->cccd.value_changed;
    } else if(type==1 || type==2) {
        key.sec.peer_addr=value->sec.peer_addr; p[0]=value->sec.peer_addr.type; memcpy(p+1,value->sec.peer_addr.val,6);
        p[7]=value->sec.key_size; ble_put16(p+8,value->sec.ediv);
        for(i=0;i<8;i++) p[10+i]=(uint8_t)(value->sec.rand_num>>(8u*i));
        memcpy(p+18,value->sec.ltk,16); memcpy(p+34,value->sec.irk,16); memcpy(p+50,value->sec.csrk,16);
        p[66]=(uint8_t)(value->sec.ltk_present | value->sec.irk_present<<1 | value->sec.csrk_present<<2 |
            value->sec.authenticated<<3 | value->sec.sc<<4);
    } else return BLE_HS_ENOTSUP;
    {
        ble_addr_t a; a.type=p[0]; memcpy(a.val,p+1,6);
        if(!identity(&a)) { bonds.d.private_refused++; return BLE_HS_EINVAL; }
        p[0]=a.type;
        if(type==3) key.cccd.peer_addr=a; else key.sec.peer_addr=a;
    }
    found=find(type,&key);
    /* Reserved security payload bytes 68..71 are a per-peer cache generation.
     * Old FLBD v1 records contain zero; keep their keys/CCCDs and treat them as
     * stale. Repeated key writes must never mark an old cache as current. */
    if(type<3) ble_put32(p+68,found<0 ? BLE_GATT_DB_VERSION :
                        ble_get32(bonds.entry[found].p+68));
    if(found<0) {
        for(i=0;i<BOND_COUNT;i++) if(bonds.entry[i].active && bonds.entry[i].type==type) count++;
        if(type<3 && count==2) return BLE_HS_ESTORE_CAP;
        /* A delete followed by recreation of the same key keeps its slot,
         * even when an earlier unused slot exists. This preserves net state. */
        for(i=0;i<BOND_COUNT;i++) if(!bonds.entry[i].active && bonds.entry[i].type==type &&
            !memcmp(bonds.entry[i].p,p,7) && (type!=3 || !memcmp(bonds.entry[i].p+8,p+8,2))) break;
        if(i==BOND_COUNT) for(i=0;i<BOND_COUNT;i++) if(!bonds.entry[i].active) break;
        if(i==BOND_COUNT) return BLE_HS_ESTORE_CAP;
        found=(int)i;
    }
    if(bonds.entry[found].active && bonds.entry[found].type==type && !memcmp(bonds.entry[found].p,p,80)) { bonds.d.net_noops++; return 0; }
    bonds.entry[found].type=(uint8_t)type; bonds.entry[found].active=1; bonds.entry[found].dirty=1;
    memcpy(bonds.entry[found].p,p,80); reconcile(); return 0;
}
int ble_bonds_delete(int type,const union ble_store_key *key)
{
    int i=find(type,key);
    if(i<0) return BLE_HS_ENOENT;
    bonds.entry[i].active=0; reconcile(); return 0;
}
static int db_entry(uint8_t type,const uint8_t identity[6])
{
    union ble_store_key key;
    memset(&key,0,sizeof key); key.sec.peer_addr.type=type;
    memcpy(key.sec.peer_addr.val,identity,6);
    return find(BLE_STORE_OBJ_TYPE_OUR_SEC,&key);
}
int ble_bonds_db_version(uint8_t type,const uint8_t identity[6],uint32_t *version)
{
    int i=db_entry(type,identity);
    if(i<0) return BLE_HS_ENOENT;
    *version=ble_get32(bonds.entry[i].p+68); return 0;
}
int ble_bonds_db_confirm(uint8_t type,const uint8_t identity[6],uint32_t version)
{
    int i=db_entry(type,identity);
    if(i<0) return BLE_HS_ENOENT;
    ble_put32(bonds.entry[i].p+68,version); reconcile(); return 0;
}
#if FM1B_BLE_OPEN
void ble_bonds_flush(void)
{
    uint8_t records[BLE_BOND_SLOTS*128u]; unsigned i;
    if(!bonds.d.pending || !ble_port_storage_idle()) return;
    /* Persist net state as a single transaction; no destructive compaction. */
    for(i=0;i<BOND_COUNT;i++) if(bonds.entry[i].active && bonds.entry[i].type==3) {
        unsigned j;
        for(j=0;j<BOND_COUNT;j++) if(bonds.entry[j].active && bonds.entry[j].type<3 &&
            !memcmp(bonds.entry[j].p,bonds.entry[i].p,7)) break;
        if(j==BOND_COUNT) { bonds.entry[i].active=0; bonds.d.orphan_dropped++; }
    }
    reconcile();
    if(!bonds.d.pending) return;
    memset(records,255,sizeof records);
    for(i=0;i<BOND_COUNT;i++) if(bonds.entry[i].active) {
        uint8_t *r=records+i*128u;
        memset(r,0,128); memcpy(r,"FLBD",4); r[4]=1; r[5]=bonds.entry[i].type;
        r[6]=(uint8_t)i; r[7]=1; ble_put32(r+8,bonds.sequence+1u);
        memcpy(r+12,bonds.entry[i].p,80); ble_put32(r+124,ble_crc32(r,124));
    }
    if(ble_object_bonds_save(records)) { bonds.d.failures++; return; }
    bonds.sequence++; bonds.d.writes++; bonds.d.erases++;
    for(i=0;i<BOND_COUNT;i++) {
        bonds.entry[i].saved_type=bonds.entry[i].type;
        bonds.entry[i].saved_active=bonds.entry[i].active;
        memcpy(bonds.entry[i].saved,bonds.entry[i].p,80);
    }
    reconcile();
}
#else
void ble_bonds_flush(void)
{
    uint8_t record[128],readback[128]; uint32_t ticks; unsigned i,dirty=0; int rc;
    if(!bonds.d.pending || !ble_port_storage_idle()) return;
    for(i=0;i<BOND_COUNT;i++) dirty+=bonds.entry[i].dirty;
    if(bonds.next+dirty>30u) {
        /* A CCCD is meaningful only while that identity has security state. */
        for(i=0;i<BOND_COUNT;i++) if(bonds.entry[i].active && bonds.entry[i].type==3) {
            unsigned j;
            for(j=0;j<BOND_COUNT;j++) if(bonds.entry[j].active && bonds.entry[j].type<3 &&
                !memcmp(bonds.entry[j].p,bonds.entry[i].p,7)) break;
            if(j==BOND_COUNT) { bonds.entry[i].active=0; bonds.d.orphan_dropped++; }
        }
        rc=ble_port_flash_erase(BOND_BASE,&ticks);
        if(rc) { bonds.d.failures++; return; }
        bonds.d.erases++; bonds.next=0;
        for(i=0;i<BOND_COUNT;i++) bonds.entry[i].saved_active=0;
        reconcile();
    }
    for(i=0;i<BOND_COUNT;i++) if(bonds.entry[i].dirty) {
        memset(record,0,sizeof record); memcpy(record,"FLBD",4); record[4]=1; record[5]=bonds.entry[i].type;
        record[6]=(uint8_t)i; record[7]=bonds.entry[i].active; ble_put32(record+8,++bonds.sequence);
        memcpy(record+12,bonds.entry[i].p,80); ble_put32(record+124,ble_crc32(record,124));
        rc=ble_port_flash_program(BOND_BASE+bonds.next*128u,record,128,&ticks); bonds.next++;
        if(rc || ble_port_flash_read(BOND_BASE+(bonds.next-1u)*128u,readback,128) || memcmp(record,readback,128)) {
            bonds.d.failures++; return;
        }
        bonds.entry[i].dirty=0; bonds.d.writes++;
        bonds.entry[i].saved_type=bonds.entry[i].type; bonds.entry[i].saved_active=bonds.entry[i].active;
        memcpy(bonds.entry[i].saved,bonds.entry[i].p,80);
    }
    bonds.d.pending=0;
}
#endif
void ble_bonds_get_diag(ble_bond_diag_t *out)
{
    unsigned i; *out=bonds.d; out->records=0;
    for(i=0;i<BOND_COUNT;i++) out->records+=bonds.entry[i].active;
}

#endif /* original two-peer sector journal */
