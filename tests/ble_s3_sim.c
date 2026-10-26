/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#include <assert.h>
#include <string.h>
#include "tinycrypt/aes.h"
#include "tinycrypt/sha256.h"
#include "ble_s3_sim.h"
#include "../open_ble/ble_seed.h"
#include "../open_ble/ble_crypto.h"
#include "../open_ble/ble_reliability.h"
uint8_t sim_flash[12288];
int sim_flash_fault,sim_flash_idle=1,sim_flash_mismatch,sim_aes_fault;
int sim_aes_wrong_blocks;
#if FM1B_BLE_RELIABILITY && defined(BLE_MIC_RADIO)
uint32_t sim_sw_aes_cost_ticks,sim_sw_key_cost_ticks;
static unsigned sim_aes_hardware;
extern void (*sim_sw_aes_hook)(void);
int __real_tc_aes_encrypt(uint8_t *,const uint8_t *,const TCAesKeySched_t);
int __wrap_tc_aes_encrypt(uint8_t *out,const uint8_t *in,const TCAesKeySched_t key)
{
    if(!sim_aes_hardware) {
        sim_ticks+=sim_sw_aes_cost_ticks;
        if(sim_sw_aes_hook) { void (*hook)(void)=sim_sw_aes_hook; sim_sw_aes_hook=0; hook(); }
    }
    return __real_tc_aes_encrypt(out,in,key);
}
int __real_tc_aes128_set_encrypt_key(TCAesKeySched_t,const uint8_t *);
int __wrap_tc_aes128_set_encrypt_key(TCAesKeySched_t key,const uint8_t *in)
{
    if(!sim_aes_hardware) sim_ticks+=sim_sw_key_cost_ticks;
    return __real_tc_aes128_set_encrypt_key(key,in);
}
#endif
unsigned sim_erases[3],sim_programs[3];
static struct tc_aes_key_sched_struct aes;
#if FM1B_BLE_RELIABILITY
static ble_aes_state_t aes_state;
void ble_port_aes_state(ble_aes_state_t *out) { *out=aes_state; }
#endif
#ifdef BLE_MIC_RADIO
static int aes_busy;
static uint32_t aes_lock;
#endif
uint32_t sim_sha_compressions,sim_sha_cost_ticks;
uint32_t sim_aes_blocks,sim_aes_cost_ticks,sim_memory_cost_ticks;
void *__real_memset(void *,int,size_t);
void *__real_memcpy(void *,const void *,size_t);
void __real_ble_secure_zero(void *,unsigned);
void *__wrap_memset(void *p,int value,size_t n)
{
    void *out=__real_memset(p,value,n); sim_ticks+=(uint32_t)n*sim_memory_cost_ticks; return out;
}
void *__wrap_memcpy(void *p,const void *s,size_t n)
{
    void *out=__real_memcpy(p,s,n); sim_ticks+=(uint32_t)n*sim_memory_cost_ticks; return out;
}
void __wrap_ble_secure_zero(void *p,unsigned n)
{
    __real_ble_secure_zero(p,n); sim_ticks+=n*sim_memory_cost_ticks;
}
int __real_tc_sha256_update(TCSha256State_t s,const uint8_t *p,size_t n);
int __real_tc_sha256_final(uint8_t *p,TCSha256State_t s);
int __wrap_tc_sha256_update(TCSha256State_t s,const uint8_t *p,size_t n)
{
    unsigned blocks=(unsigned)((s->leftover_offset+n)/64u);
    int rc=__real_tc_sha256_update(s,p,n);
    sim_sha_compressions+=blocks; sim_ticks+=blocks*sim_sha_cost_ticks; return rc;
}
int __wrap_tc_sha256_final(uint8_t *p,TCSha256State_t s)
{
    unsigned blocks=s->leftover_offset>=56 ? 2u : 1u;
    int rc=__real_tc_sha256_final(p,s);
    sim_sha_compressions+=blocks; sim_ticks+=blocks*sim_sha_cost_ticks; return rc;
}
int ble_port_aes_begin(const uint8_t key[16])
{
#ifdef BLE_MIC_RADIO
    aes_lock=ble_port_lock(); assert(!aes_busy); aes_busy=1;
#endif
    #if FM1B_BLE_RELIABILITY
    aes_state.busy=1; aes_state.epoch=ble_link.epoch; aes_state.operations++;
#endif
    uint32_t cost=sim_memory_cost_ticks; int rc;
    sim_memory_cost_ticks=0;
#if FM1B_BLE_RELIABILITY && defined(BLE_MIC_RADIO)
    sim_aes_hardware++;
#endif
    rc=!sim_aes_fault && tc_aes128_set_encrypt_key(&aes,key);
#if FM1B_BLE_RELIABILITY && defined(BLE_MIC_RADIO)
    sim_aes_hardware--;
#endif
    sim_memory_cost_ticks=cost; return rc;
}
int ble_port_aes_block(const uint8_t in[16],uint8_t out[16])
{
    uint32_t cost=sim_memory_cost_ticks; int rc;
    sim_aes_blocks++; sim_ticks+=sim_aes_cost_ticks;
#if FM1B_BLE_RELIABILITY
    aes_state.blocks++;
#endif
    /* TinyCrypt simulates the AES peripheral. Its internal software copies
     * are not CPU work in the target driver; charge the block once. */
    sim_memory_cost_ticks=0;
#if FM1B_BLE_RELIABILITY && defined(BLE_MIC_RADIO)
    sim_aes_hardware++;
#endif
    rc=!sim_aes_fault && tc_aes_encrypt(out,in,&aes);
#if FM1B_BLE_RELIABILITY && defined(BLE_MIC_RADIO)
    sim_aes_hardware--;
#endif
    if(rc && sim_aes_wrong_blocks) { out[0]^=1; if(sim_aes_wrong_blocks>0) sim_aes_wrong_blocks--; }
    sim_memory_cost_ticks=cost; return rc;
}
void ble_port_aes_end(void) {
#if FM1B_BLE_RELIABILITY
    aes_state.busy=0; aes_state.last_epoch=aes_state.epoch;
#endif
#ifdef BLE_MIC_RADIO
    assert(aes_busy); aes_busy=0; ble_port_unlock(aes_lock);
#endif
}
int ble_port_storage_idle(void) { return sim_flash_idle && ble_link.state==BLE_LINK_IDLE; }
int ble_port_flash_read(uint32_t off,uint8_t *out,uint32_t n)
{
    assert(off>=0x94000u && n<=0x97000u-off); memcpy(out,sim_flash+off-0x94000u,n); return 0;
}
int ble_port_flash_erase(uint32_t off,uint32_t *ticks)
{
    unsigned area=(off-0x94000u)/4096u;
    assert(off>=0x94000u && area<3 && !(off&4095u)); *ticks=24000;
    if(!ble_port_storage_idle()) return BLE_FLASH_REFUSED;
    if(sim_flash_fault) return sim_flash_fault;
    memset(sim_flash+area*4096u,255,4096); sim_erases[area]++; return 0;
}
int ble_port_flash_program(uint32_t off,const uint8_t *p,uint32_t n,uint32_t *ticks)
{
    unsigned i,area=(off-0x94000u)/4096u;
    assert(off>=0x94000u && area<3 && n && n<=256 && (off&255u)+n<=256 && (off&4095u)+n<=3840);
    *ticks=2400;
    if(!ble_port_storage_idle()) return BLE_FLASH_REFUSED;
    if(sim_flash_fault==1 || sim_flash_fault==2) return sim_flash_fault;
    sim_programs[area]++;
    for(i=0;i<n;i++) { assert((sim_flash[off-0x94000u+i]&p[i])==p[i]); sim_flash[off-0x94000u+i]&=p[i]; }
    if(sim_flash_mismatch) sim_flash[off-0x94000u]^=1;
    return sim_flash_fault;
}
void sim_storage_reset(void)
{
    memset(sim_flash,255,sizeof sim_flash); memset(sim_erases,0,sizeof sim_erases); memset(sim_programs,0,sizeof sim_programs);
    sim_flash_fault=sim_flash_mismatch=sim_aes_fault=0; sim_flash_idle=1;
#if FM1B_BLE_OPEN
    extern void sim_object_reset(void); sim_object_reset();
#endif
}
void sim_seed(void)
{
    static const uint8_t address[6]={0x58,0x65,0x67,0x8b,0x40,0x62};
#if FM1B_BLE_OPEN
    extern void ble_object_init(void); ble_object_init();
#endif
    ble_seed_boot(address,1,2,3);
}
