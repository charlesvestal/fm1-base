/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* r4 04 §8.1. Hash counter generator; all physical inputs carry zero credit.
 * Seed flash is accessed only by boot. Neither HCI Reset nor reseed writes it. */
#include "ble_seed.h"
#if FM1B_BLE_OPEN
#include "ble_store.h"
#endif
#include "ble_crypto.h"
#include "ble_link.h"
#include "ble_memory.h"
#include "ble_timing.h"
#include "tinycrypt/sha256.h"
static struct {
    uint8_t pool[32],key[32]; uint32_t counter,since_reseed;
    volatile uint8_t connection_reseed;
    ble_seed_diag_t diag;
    struct { uint32_t ticks,rssi; } samples[8];
    volatile uint32_t w,r;
    uint32_t previous_tick;
} rng __attribute__((section(".pool.ble")));
static struct {
    struct tc_sha256_state_struct hash;
    uint8_t input[70],block[32],result[32];
    uint8_t owner,n,stage,phase,length,discard;
} random_job __attribute__((section(".pool.ble")));
uint32_t ble_crc32(const uint8_t *p,unsigned n)
{
    uint32_t crc=0xffffffffu; unsigned j;
    while(n--) { crc^=*p++; for(j=0;j<8;j++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u); }
    return ~crc;
}
static int blank(const uint8_t *p,unsigned n)
{
    while(n--) if(*p++!=255) return 0;
    return 1;
}
int ble_seed_record_valid(const uint8_t p[64])
{
    unsigned i;
    if(memcmp(p,"WLSD",4) || p[4]!=1 || p[5]>1 || ble_get16(p+6)>256 ||
        (!p[5] && ble_get16(p+6)) || ble_get32(p+60)!=ble_crc32(p,60)) return 0;
    for(i=44;i<60;i++) if(p[i]) return 0;
    return 1;
}
static void hash_parts(uint8_t out[32],uint8_t label,const uint8_t *a,unsigned an,const uint8_t *b,unsigned bn)
{
    struct tc_sha256_state_struct h; uint8_t header[3]={label,0,0};
    tc_sha256_init(&h); ble_put16(header+1,(uint16_t)an); tc_sha256_update(&h,header,3);
    if(an) tc_sha256_update(&h,a,an);
    header[0]=(uint8_t)(label^0x80u); ble_put16(header+1,(uint16_t)bn); tc_sha256_update(&h,header,3);
    if(bn) tc_sha256_update(&h,b,bn);
    tc_sha256_final(out,&h); ble_secure_zero(&h,sizeof h);
}
void ble_entropy_absorb(uint8_t label,const void *p,unsigned n)
{
    hash_parts(rng.pool,label,rng.pool,32,p,n);
}
static int hash_selftest(void)
{
    static const uint8_t answer[32]={0xba,0x78,0x16,0xbf,0x8f,1,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,3,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0,0x15,0xad};
    struct tc_sha256_state_struct h; uint8_t digest[32];
    tc_sha256_init(&h); tc_sha256_update(&h,(const uint8_t *)"abc",3); tc_sha256_final(digest,&h);
    return !memcmp(digest,answer,32);
}
static int drbg_selftest(void)
{
    static const uint8_t expected[32]={0x34,4,0x47,0xb9,0xbe,0x2a,0xcb,0xfe,0xb8,0x98,0xcd,0xf3,0x85,0x93,0x3d,0x14,
        0x8c,0x1c,0xfb,5,0x3a,0xe3,0xfa,0xb8,0x90,0x41,0xbc,0x41,0xa3,0xc5,0x5f,0x80};
    uint8_t key[32],counter[4]={1,0,0,0},out[32]; unsigned i;
    for(i=0;i<32;i++) key[i]=(uint8_t)i;
    hash_parts(out,0x31,key,32,counter,4); return !memcmp(out,expected,32);
}
void ble_seed_boot(const uint8_t address[6],uint32_t reset,uint32_t r64lo,uint32_t r64hi)
{
    uint8_t candidate[64],record[64],old[64]={0},words[12];
    int selected=-1,rc;
    uint32_t duration=0; uint8_t retry=0;
#if !FM1B_BLE_OPEN
    int last[2]={-1,-1},any[2]={0,0},target,slot,erase=0;
    unsigned area,i; uint32_t off;
#endif
    memset(&rng,0,sizeof rng);
    ble_secure_zero(&random_job,sizeof random_job);
    if(!hash_selftest() || !drbg_selftest() || !ble_crypto_selftest()) { rng.diag.state=BLE_SEED_FAILED; return; }
#if FM1B_BLE_OPEN
    if(!ble_object_seed_load(candidate) && !blank(candidate,64)) {
        if(ble_seed_record_valid(candidate)) { memcpy(old,candidate,64); selected=0; }
        else rng.diag.malformed++;
    }
#else
    for(area=0;area<2;area++) for(i=0;i<60;i++) {
        if(ble_port_flash_read(0x94000u+area*4096u+i*64u,candidate,64)) continue;
        if(blank(candidate,64)) continue;
        last[area]=(int)i; any[area]=1;
        if(!ble_seed_record_valid(candidate)) { rng.diag.malformed++; continue; }
        if(selected<0) { memcpy(old,candidate,64); selected=(int)area; }
        else {
            uint32_t delta=ble_get32(candidate+8)-ble_get32(old+8);
            if(!delta || delta==0x80000000u) rng.diag.conflicts++;
            else if(delta<0x80000000u) { memcpy(old,candidate,64); selected=(int)area; }
        }
    }
#endif
    rng.diag.origin=selected<0 ? BLE_SEED_FIRST_BOOT :
        old[5] && ble_get16(old+6)==256 ? BLE_SEED_PROVISIONED : BLE_SEED_CHAINED;
    if(selected>=0) {
        ble_entropy_absorb(1,old+12,32); ble_entropy_absorb(2,old+8,4);
        rng.diag.flags=old[5]; rng.diag.credit=ble_get16(old+6);
        rng.diag.sequence=ble_get32(old+8)+1u;
    } else rng.diag.sequence=1;
    ble_entropy_absorb(3,address,6); ble_put32(words,reset); ble_entropy_absorb(4,words,4);
    /* Optional power-on RAM is omitted: no Felucca window has been qualified.
     * One actual boot-time reading is a diversifier, never a timing sample loop. */
    ble_put32(words,ble_port_ticks()); ble_entropy_absorb(5,words,4);
    ble_put32(words,r64lo); ble_put32(words+4,r64hi); ble_entropy_absorb(6,words,8);
    memset(record,0,64); memcpy(record,"WLSD",4); record[4]=1; record[5]=rng.diag.flags;
    ble_put16(record+6,rng.diag.credit); ble_put32(record+8,rng.diag.sequence);
    do { hash_parts(record+12,0x20,rng.pool,32,&retry,1); retry++; }
    while(selected>=0 && !memcmp(record+12,old+12,32) && retry);
    hash_parts(rng.key,0x21,rng.pool,32,0,0); rng.counter=1;
    ble_put32(record+60,ble_crc32(record,60));
#if FM1B_BLE_OPEN
    rc=ble_object_seed_save(record,&duration);
#else
    if(selected>=0) {
        target=selected; slot=last[target]+1;
        if(slot==60) { target^=1; slot=0; erase=1; }
    } else { target=any[0] && !any[1] ? 1 : 0; slot=0; erase=any[target]; }
    off=0x94000u+(uint32_t)target*4096u+(uint32_t)slot*64u;
    rc=erase ? ble_port_flash_erase(off&~4095u,&duration) : BLE_FLASH_OK;
    if(rc==BLE_FLASH_OK) rc=ble_port_flash_program(off,record,64,&duration);
#endif
    rng.diag.persistence=rc==BLE_FLASH_OK ? BLE_PERSIST_STORED :
        (rc==BLE_FLASH_REFUSED || rc==BLE_FLASH_NOT_ISSUED) ? BLE_PERSIST_NOT_STORED : BLE_PERSIST_UNVERIFIED;
    if(rc==BLE_FLASH_OK && (
#if FM1B_BLE_OPEN
        ble_object_seed_load(candidate)
#else
        ble_port_flash_read(off,candidate,64)
#endif
        || memcmp(candidate,record,64)))
        rng.diag.persistence=BLE_PERSIST_UNVERIFIED;
    rng.diag.state=rng.diag.persistence==BLE_PERSIST_STORED ? rng.diag.origin : BLE_SEED_UNPERSISTED;
    if(rng.diag.persistence!=BLE_PERSIST_STORED) rng.diag.flash_failures++;
    ble_put32(words,duration); ble_entropy_absorb(7,words,4);
    ble_secure_zero(old,sizeof old); ble_secure_zero(record,sizeof record); ble_secure_zero(candidate,sizeof candidate);
}
void ble_entropy_connection(void) { rng.connection_reseed=1; }
void ble_entropy_sample(uint32_t ticks,uint32_t rssi,int event)
{
    uint32_t w=rng.w;
    if(w-rng.r==8) return;
    rng.samples[w%8u].ticks=event ? ticks-rng.previous_tick : 0; rng.samples[w%8u].rssi=rssi;
    if(event) rng.previous_tick=ticks;
    __asm__ volatile("" ::: "memory"); rng.w=w+1u;
}
void ble_entropy_poll(void)
{
    if(rng.r!=rng.w) {
        uint32_t words[2],r=rng.r;
        words[0]=rng.samples[r%8u].ticks; words[1]=rng.samples[r%8u].rssi;
        rng.r=r+1u; ble_entropy_absorb(8,words,sizeof words);
    }
}
int ble_entropy(uint8_t *out,unsigned n)
{
    uint8_t block[32],counter[4]; unsigned take;
    if(rng.diag.state==BLE_SEED_FAILED || random_job.stage) return 0;
    if(rng.connection_reseed || rng.since_reseed>=65536u) {
        hash_parts(rng.key,0x30,rng.key,32,rng.pool,32);
        rng.connection_reseed=0; rng.since_reseed=0; rng.diag.reseed_attempts++;
    }
    rng.diag.bytes+=n; rng.since_reseed+=n;
    while(n) {
        ble_put32(counter,rng.counter++); hash_parts(block,0x31,rng.key,32,counter,4);
        take=n>32 ? 32 : n; memcpy(out,block,take); out+=take; n-=take;
        hash_parts(rng.key,0x32,rng.key,32,block,32);
        if(!rng.counter) rng.counter=1; /* a freshly rekeyed generator, no nonce reuse */
    }
    ble_secure_zero(block,sizeof block); return 1;
}
/* Preserve hash_parts' byte framing and generator rekeying. Each call to
 * step executes at most ONE TinyCrypt SHA compression, even for a 70-byte
 * reseed/rekey input (update 64 bytes, then finalise the remaining six). */
static void random_hash(uint8_t label,const uint8_t *a,unsigned an,const uint8_t *b,unsigned bn)
{
    uint8_t *p=random_job.input;
    p[0]=label; ble_put16(p+1,(uint16_t)an); memcpy(p+3,a,an);
    p[3+an]=(uint8_t)(label^0x80u); ble_put16(p+4+an,(uint16_t)bn);
    if(bn) memcpy(p+6+an,b,bn);
    random_job.length=(uint8_t)(6u+an+bn); random_job.phase=0;
    tc_sha256_init(&random_job.hash);
}
int ble_entropy_request(unsigned owner,uint8_t *out,unsigned n)
{
    if(rng.diag.state==BLE_SEED_FAILED || !owner || owner>BLE_RANDOM_LINK || !n || n>32) return -1;
    if(random_job.stage) {
        if(random_job.owner!=owner || random_job.discard) return 0;
        if(random_job.n!=n) return -1;
        if(random_job.stage!=4) return 0;
        memcpy(out,random_job.result,n); ble_secure_zero(&random_job,sizeof random_job); return 1;
    }
    random_job.owner=(uint8_t)owner; random_job.n=(uint8_t)n;
    if(rng.connection_reseed || rng.since_reseed>=65536u) {
        random_job.stage=1; rng.connection_reseed=0; rng.since_reseed=0; rng.diag.reseed_attempts++;
        random_hash(0x30,rng.key,32,rng.pool,32);
    } else {
        uint8_t counter[4]; ble_put32(counter,rng.counter++);
        random_job.stage=2; random_hash(0x31,rng.key,32,counter,4);
    }
    ble_port_pend(); return 0;
}
void ble_entropy_cancel(unsigned owner)
{
    if(random_job.owner!=owner) return;
    if(random_job.stage==4) ble_secure_zero(&random_job,sizeof random_job);
    else random_job.discard=1; /* Finish rekey; never rewind consumed counters. */
}
void ble_entropy_step(void)
{
    uint32_t item; ble_work_kind_t kind; uint8_t digest[32];
    if(!random_job.stage || random_job.stage==4) return;
    kind=random_job.stage==1 ? BLE_WORK_ENTROPY_RESEED :
        random_job.stage==2 ? BLE_WORK_ENTROPY_OUTPUT : BLE_WORK_ENTROPY_REKEY;
    item=ble_time_item_begin();
    if(!random_job.phase && random_job.length>=64) {
        tc_sha256_update(&random_job.hash,random_job.input,64); random_job.phase=1;
    } else {
        unsigned offset=random_job.phase ? 64u : 0;
        tc_sha256_update(&random_job.hash,random_job.input+offset,random_job.length-offset);
        tc_sha256_final(digest,&random_job.hash);
        if(random_job.stage==1) {
            uint8_t counter[4]; memcpy(rng.key,digest,32); ble_put32(counter,rng.counter++);
            random_job.stage=2; random_hash(0x31,rng.key,32,counter,4);
        } else if(random_job.stage==2) {
            memcpy(random_job.result,digest,random_job.n);
            memcpy(random_job.block,digest,32); random_job.stage=3;
            random_hash(0x32,rng.key,32,random_job.block,32);
        } else {
            memcpy(rng.key,digest,32); if(!rng.counter) rng.counter=1;
            rng.diag.bytes+=random_job.n; rng.since_reseed+=random_job.n;
            random_job.stage=4;
        }
    }
    ble_secure_zero(digest,sizeof digest);
    ble_time_item_end(item,kind,random_job.owner==BLE_RANDOM_HCI ? 0x2018 : 0);
    if(random_job.stage==4 && random_job.discard) ble_secure_zero(&random_job,sizeof random_job);
    if(random_job.stage && random_job.stage!=4) ble_port_pend();
}
void ble_seed_rand_called(void) { rng.diag.rand_calls++; }
void ble_seed_reset_stats(void)
{
    rng.diag.rand_calls=rng.diag.bytes=rng.diag.reseed_attempts=0;
    /* Boot parsing and persistence results describe the surviving seed state. */
}
void ble_seed_get_diag(ble_seed_diag_t *out)
{
    uint32_t key=ble_port_lock(); *out=rng.diag; ble_port_unlock(key);
}
