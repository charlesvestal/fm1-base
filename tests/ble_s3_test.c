/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ble_s3_sim.h"
#include "../open_ble/ble_crypto.h"
#include "../open_ble/ble_seed.h"
#include "../open_ble/ble_security.h"
#include "../open_ble/ble_bonds.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
static unsigned unhex(uint8_t *out,const char *s)
{
    unsigned x,n=0; while(*s) { assert(sscanf(s,"%2x",&x)==1); out[n++]=(uint8_t)x; s+=2; } return n;
}
static void crypto(void)
{
    const char *clear[]={"06","06","1700636465666768696a6b6c6d6e6f707131323334353637383930","170037363534333231304142434445464748494a4b4c4d4e4f5051"};
    const char *air[]={"9fcda7f448","a34c13a415","7a70d66415226df26b17839a060405596bd6564f796b5b9ce6ff32f75a6d33","f38881e7bd94c9c369b9a66846dd4786aa8c39ce540d0dae3adcdf89b96088"};
    uint8_t key[16],iv[8],plain[31],p[31],want[31],saved[31]; unsigned i,n;
    assert(ble_crypto_selftest()); unhex(key,"99ad1b5226a37e3e058e3b8e27c2c666"); unhex(iv,"24abdcbabebaafde");
    for(i=0;i<4;i++) {
        n=unhex(plain,clear[i]); memcpy(p,plain,n); unhex(want,air[i]);
        assert(ble_ccm(key,iv,i/2u,!(i&1u),i<2 ? 3 : 2,p,(uint8_t)n,0)); assert(!memcmp(p,want,n+4));
        assert(ble_ccm(key,iv,i/2u,!(i&1u),i<2 ? 3 : 2,p,(uint8_t)n,1)); assert(!memcmp(p,plain,n));
        memcpy(p,want,n+4); memcpy(saved,p,n+4);
        assert(!ble_ccm(key,iv,i/2u+7u,!(i&1u),i<2 ? 3 : 2,p,(uint8_t)n,1)); assert(!memcmp(p,saved,n+4));
        assert(!ble_ccm(key,iv,i/2u,(i&1u),i<2 ? 3 : 2,p,(uint8_t)n,1));
        assert(!ble_ccm(key,iv,i/2u,!(i&1u),1,p,(uint8_t)n,1));
    }
    for(i=0;i<10000;i++) { p[0]=6; assert(ble_ccm(key,iv,i,1,3,p,1,0)); assert(ble_ccm(key,iv,i,1,3,p,1,1) && p[0]==6); }
    assert(!ble_ccm(key,iv,(uint64_t)1<<39,1,3,p,1,0));
}
static uint8_t hci_event[80]; static uint16_t hci_n;
static void hci_receive(uint8_t type,const uint8_t *p,uint16_t n)
{
    assert(type==4 && n<=80); memcpy(hci_event,p,n); hci_n=n;
}
static uint8_t hci(uint16_t op,const uint8_t *p,unsigned n)
{
    uint8_t command[35]; ble_put16(command,op); command[2]=(uint8_t)n; if(n) memcpy(command+3,p,n);
    hci_n=0; assert(!ble_hci_send(1,command,(uint16_t)(n+3u)));
    for(unsigned i=0;i<64 && !hci_n;i++) { ble_ll_worker(); ble_worker(); }
    assert(hci_event[0]==14 && ble_get16(hci_event+3)==op); return hci_event[5];
}
static void hci_crypto(void)
{
    uint8_t data[32],expected[16],previous[8]; ble_seed_diag_t before,after;
    static const uint8_t pub[6]={0x58,0x65,0x67,0x8b,0x40,0x62};
    sim_reset(); sim_storage_reset(); sim_seed(); ble_init(pub,hci_receive);
    unhex(data,"bf01fb9d4ef3bc36d874f5394138684c1302f1e0dfcebdac7968574635241302");
    unhex(expected,"66c6c2278e3b8e053e7ea326521bad99");
    assert(!hci(0x2017,data,32) && hci_n==22 && !memcmp(hci_event+6,expected,16));
    assert(!hci(0x2018,0,0) && hci_n==14); memcpy(previous,hci_event+6,8);
    assert(!hci(0x2018,0,0) && memcmp(previous,hci_event+6,8));
    assert(!hci(0x2003,0,0) && hci_event[6]==1);
    assert(hci(0x2025,0,0)==1 && hci(0x2026,0,0)==1 && hci(0x2019,0,0)==1);
    ble_seed_get_diag(&before); assert(!hci(0x0c03,0,0)); ble_seed_get_diag(&after);
    assert(before.sequence==after.sequence && !after.rand_calls && sim_programs[0]==1);
    assert(!hci(0x2018,0,0) && memcmp(previous,hci_event+6,8));
    sim_aes_fault=1; sim_seed(); assert(hci(0x2018,0,0)==3 && hci_n==6); sim_aes_fault=0;
}
static void record(uint8_t *p,uint32_t sequence,int provisioned)
{
    unsigned i; memset(p,0,64); memcpy(p,"WLSD",4); p[4]=1; p[5]=(uint8_t)provisioned;
    ble_put16(p+6,provisioned ? 256 : 0); ble_put32(p+8,sequence);
    for(i=0;i<32;i++) p[12+i]=provisioned ? 0xa5 : (uint8_t)i;
    ble_put32(p+60,ble_crc32(p,60));
}
static void seed(void)
{
    uint8_t r[64],a[8],b[8]; ble_seed_diag_t d; unsigned i;
    assert(ble_crc32((const uint8_t *)"123456789",9)==0xcbf43926u);
    record(r,1,0); assert(ble_get32(r+60)==0xe4c8e518u && ble_seed_record_valid(r));
    record(r,0xffffffffu,1); assert(ble_get32(r+60)==0x11c6de7au && ble_seed_record_valid(r));
    for(i=0;i<5;i++) {
        record(r,1,0);
        if(i==0) r[4]=2;
        if(i==1) r[5]=2;
        if(i==2) r[44]=1;
        if(i==3) ble_put16(r+6,300);
        if(i==4) ble_put16(r+6,1);
        ble_put32(r+60,ble_crc32(r,60)); assert(!ble_seed_record_valid(r));
    }
    sim_reset(); sim_storage_reset();
    for(i=1;i<=241;i++) {
        sim_seed(); ble_seed_get_diag(&d); assert(d.sequence==i && d.persistence==0 && d.state==(i==1 ? 1 : 2));
        assert(ble_entropy(a,8)); if(i>1) assert(memcmp(a,b,8)); memcpy(b,a,8);
    }
    assert(sim_erases[0]==2 && sim_erases[1]==2 && sim_programs[0]+sim_programs[1]==241);
    for(i=3840;i<4096;i++) assert(sim_flash[i]==255 && sim_flash[4096+i]==255);
    sim_storage_reset(); record(sim_flash,0xffffffffu,1); sim_seed(); ble_seed_get_diag(&d);
    assert(d.sequence==0 && d.state==BLE_SEED_PROVISIONED && d.credit==256);
    sim_storage_reset(); record(sim_flash,10,0); record(sim_flash+4096,10,1); record(sim_flash+4096+64,0x8000000au,1);
    sim_seed(); ble_seed_get_diag(&d); assert(d.sequence==11 && d.conflicts==2 && !d.credit);
    sim_storage_reset(); record(sim_flash,1,0); memset(sim_flash+64,0,64); sim_seed();
    assert(ble_seed_record_valid(sim_flash+128)); ble_seed_get_diag(&d); assert(d.malformed==1);
    for(i=1;i<=3;i++) {
        sim_storage_reset(); sim_flash_fault=(int)i; sim_seed(); ble_seed_get_diag(&d);
        assert(d.state==BLE_SEED_UNPERSISTED && d.persistence==(i==3 ? 2 : 1) && ble_entropy(a,8));
    }
    sim_storage_reset(); sim_flash_mismatch=1; sim_seed(); ble_seed_get_diag(&d); assert(d.persistence==2);
    sim_storage_reset(); sim_aes_fault=1; sim_seed(); ble_seed_get_diag(&d); assert(d.state==BLE_SEED_FAILED && !ble_entropy(a,8));
    /* Every torn successor prefix leaves the predecessor usable, and an
     * invalid nonblank slot is consumed rather than programmed over. */
    for(i=0;i<=64;i++) {
        sim_storage_reset(); record(sim_flash,10,0); record(r,11,0);
        memcpy(sim_flash+64,r,i); sim_seed(); ble_seed_get_diag(&d); assert(d.sequence==(i==64 ? 12u : 11u));
    }
    sim_storage_reset(); sim_seed(); assert(ble_entropy(a,8)); ble_ll_reset(); assert(ble_entropy(b,8) && memcmp(a,b,8));
    ble_entropy_connection(); assert(ble_entropy(a,8)); ble_seed_get_diag(&d); assert(d.reseed_attempts==1 && sim_programs[0]==1);
}
static void bonds(void)
{
    union ble_store_value v,out; union ble_store_key k; ble_bond_diag_t d; unsigned i;
    sim_reset(); sim_storage_reset(); ble_bonds_init(); memset(&v,0,sizeof v); memset(&k,0,sizeof k);
    v.sec.peer_addr.val[0]=1; k.sec.peer_addr=v.sec.peer_addr; v.sec.key_size=16; v.sec.ltk_present=1; v.sec.sc=1;
    for(i=0;i<80;i++) {
        v.sec.ltk[0]=(uint8_t)i; assert(!ble_bonds_write(1,&v)); sim_flash_idle=0; ble_bonds_flush();
        assert(sim_programs[2]==i); sim_flash_idle=1; ble_bonds_flush(); ble_bonds_init();
        assert(!ble_bonds_read(1,&k,&out) && out.sec.ltk[0]==i && out.sec.sc && out.sec.key_size==16);
    }
    assert(sim_erases[2]==2); assert(!ble_bonds_delete(1,&k)); ble_bonds_flush(); ble_bonds_init();
    assert(ble_bonds_read(1,&k,&out)==BLE_HS_ENOENT);
    ble_bonds_get_diag(&d); assert(!d.pending && !d.records);
    for(i=3840;i<4096;i++) assert(sim_flash[8192+i]==255);
}
static void rx(const uint8_t *p,uint8_t n,uint8_t llid)
{
    assert(ble_ll_rx(1,(uint16_t)((n<<8)|llid),p,sim_ticks,0));
    for(unsigned i=0;i<16;i++) { ble_entropy_step(); ble_ll_worker(); }
}
static int slot_opcode(uint8_t op)
{
    unsigned i; for(i=0;i<2;i++) if(sim_slot[i]>=0 && ble_ll_tx_llid((unsigned)sim_slot[i])==3 && ble_ll_tx_data((unsigned)sim_slot[i])[0]==op) return (int)i;
    return -1;
}
static uint8_t sk[16],iv[8],ltk[16],enc_req[23]; static uint64_t crx;
static void complete_start(void)
{
    uint8_t keyle[16],skd[16]; unsigned i; int slot;
    rx(enc_req,23,3); slot=slot_opcode(4); assert(slot>=0);
    memcpy(skd,enc_req+11,8); memcpy(skd+8,sim_air[slot]+1,8); memcpy(iv,enc_req+19,4); memcpy(iv+4,sim_air[slot]+9,4);
    for(i=0;i<16;i++) ltk[i]=(uint8_t)i;
    assert(ble_aes_le(ltk,skd,keyle)); for(i=0;i<16;i++) sk[i]=keyle[15-i];
    sim_ack((unsigned)slot); assert(!ble_ll_ltk_reply(BLE_HANDLE,ltk,0)); ble_ll_worker();
    slot=slot_opcode(5); assert(slot>=0);
    /* Queued allowed S1 control precedes the RX_ON marker, then encrypted RSP. */
    assert(ble_ll_rx(1,0x203,(uint8_t[]){7,4},sim_ticks,0)); sim_ack((unsigned)slot);
    { uint8_t p[5]={6}; assert(ble_ccm(sk,iv,0,1,3,p,1,0)); rx(p,5,3); }
    slot=slot_opcode(6); assert(slot>=0 && sim_air_len[slot]==5);
    { uint8_t p[5]; memcpy(p,sim_air[slot],5); assert(ble_ccm(sk,iv,0,0,3,p,1,1) && p[0]==6); }
    sim_ack((unsigned)slot); crx=1;
}
static void first(void)
{
    ble_conn_t c=sim_connection(); unsigned i;
    sim_reset(); sim_storage_reset(); sim_seed(); ble_ll_create(&c,sim_ticks);
    memset(enc_req,0,23); enc_req[0]=3; for(i=0;i<8;i++) enc_req[11+i]=(uint8_t)(11+i);
    complete_start();
}
static void encrypted(const uint8_t *plain,uint8_t n,uint8_t llid)
{
    uint8_t p[31]; memcpy(p,plain,n); assert(ble_ccm(sk,iv,crx++,1,llid,p,n,0)); rx(p,(uint8_t)(n+4u),llid);
}
static void security_tests(void)
{
    ble_security_diag_t d; unsigned i; int slot;
    first(); ble_security_get_diag(&d); assert(d.encrypted && d.rx_counter==1 && d.tx_counter==1 && d.starts==1);
    /* A valid repetition never consumes another counter. */
    assert(ble_ll_rx(9,0x503,(uint8_t[5]){0},sim_ticks,0)); ble_ll_worker(); ble_security_get_diag(&d); assert(d.rx_counter==1);
    encrypted((uint8_t[]){10},1,3); slot=slot_opcode(11); assert(slot>=0 && sim_air_len[slot]==5);
    sim_ack((unsigned)slot); rx((uint8_t[]){11},1,3); ble_security_get_diag(&d);
    assert(d.phase==BLE_ENC_PC && !d.tx_on && !d.rx_on && !d.rx_counter && !d.tx_counter);
    complete_start(); ble_security_get_diag(&d); assert(d.encrypted && d.starts==1 && d.refreshes==1);
    { uint8_t type,event[32]; uint16_t n; unsigned changed=0,refreshed=0;
      while(ble_link.created_pending) (void)ble_ll_poll(&type,event,&n);
      while(ble_ll_poll(&type,event,&n)) {
          if(event[0]==8) { assert(type==4 && n==6 && !event[2] && event[5]==1); changed++; }
          if(event[0]==0x30) { assert(type==4 && n==5 && !event[2]); refreshed++; }
      }
      assert(changed==1 && refreshed==1); }
    /* Pause again and reject the restart: terminate rather than resume clear ACL. */
    encrypted((uint8_t[]){10},1,3); slot=slot_opcode(11); assert(slot>=0); sim_ack((unsigned)slot); rx((uint8_t[]){11},1,3);
    rx(enc_req,23,3); slot=slot_opcode(4); assert(slot>=0); sim_ack((unsigned)slot);
    assert(!ble_ll_ltk_reply(BLE_HANDLE,0,1)); ble_ll_worker(); slot=slot_opcode(2); assert(slot>=0 && sim_air[slot][1]==6);
    first(); rx((uint8_t[]){2,0x13},2,3); assert(ble_link.state>=BLE_LINK_CLOSING && ble_link.reason==0x3d);
    first(); ble_security_test_counters(((uint64_t)1<<39)-1u,1); ble_port_refill(); slot=slot_opcode(2); assert(slot>=0 && sim_air_len[slot]==6);
    sim_ack((unsigned)slot); assert(ble_link.reason==0x16);
    first(); ble_security_test_counters(1,(uint64_t)1<<39); encrypted((uint8_t[]){7,0},2,3); assert(ble_link.reason==0x3d);
    /* A queued encrypted terminate before RX_OFF keeps the old nonce epoch. */
    first(); encrypted((uint8_t[]){10},1,3); slot=slot_opcode(11); assert(slot>=0);
    { uint8_t p[6]={2,0x13}; assert(ble_ccm(sk,iv,crx++,1,3,p,2,0));
      assert(ble_ll_rx(1,0x603,p,sim_ticks,0)); sim_ack((unsigned)slot);
      assert(ble_ll_rx(1,0x103,(uint8_t[]){11},sim_ticks,0)); ble_ll_worker();
      ble_security_get_diag(&d); assert(!d.mic_failures && ble_link.peer_stop && ble_link.reason==0x13); }
    /* Every unexpected LLID, empty control or ACL is fatal in S1. */
    for(i=0;i<4;i++) {
        ble_conn_t c=sim_connection(); sim_reset(); ble_ll_create(&c,sim_ticks); rx(enc_req,23,3);
        rx((uint8_t[]){6},i==0 ? 0 : 1,i==1 ? 0 : i==2 ? 2 : 3); assert(ble_link.reason==0x3d);
    }
    /* Initial negative reply retains barrier until reject acknowledgement. */
    { ble_conn_t c=sim_connection(); sim_reset(); ble_ll_create(&c,sim_ticks); rx(enc_req,23,3);
      assert(!ble_ll_ltk_reply(BLE_HANDLE,0,1)); ble_ll_worker(); slot=slot_opcode(13); assert(slot>=0);
      ble_security_get_diag(&d); assert(d.phase==BLE_ENC_S1); sim_ack((unsigned)slot); ble_ll_worker(); ble_security_get_diag(&d); assert(d.phase==BLE_ENC_N); }
}
int main(void)
{
    crypto(); seed(); hci_crypto(); bonds(); security_tests();
    puts("S3 AES/Core CCM, negative MIC/counter/LLID, seed parsing/wrap/conflicts/wear/failures, deferred persistent bonds, encryption phase/marker/barrier/exhaustion tests: PASS"); return 0;
}
