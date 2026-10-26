/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ble_s3_sim.h"
#include "ble_privacy.h"
#include "ble_crypto.h"
#include "ble_bonds.h"
#include "ble_host.h"
#include "ble_seed.h"
#include "ble_timing.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
static const uint8_t pub[6]={0x58,0x65,0x67,0x8b,0x40,0x62};
static const uint8_t irk[16]={0x9b,0x7d,0x39,0x0a,0xa6,0x10,0x10,0x34,5,0xad,0xc8,0x57,0xa3,0x34,2,0xec};
static const uint8_t rpa[6]={0xaa,0xfb,0x0d,0x94,0x81,0x70};
static uint8_t reply[80],connection[40];
static unsigned reply_count,connection_n,connection_events;
static void receive(uint8_t type,const uint8_t *p,uint16_t n)
{
    assert(type==4 && n<=80);
    if(p[0]==14) { memcpy(reply,p,n); reply_count++; }
    if(p[0]==0x3e && (p[2]==1 || p[2]==10)) { memcpy(connection,p,n); connection_n=n; connection_events++; }
}
static uint8_t hci(uint16_t op,const void *p,unsigned n)
{
    uint8_t b[80]; unsigned i,count=reply_count;
    ble_put16(b,op); b[2]=(uint8_t)n; if(n) memcpy(b+3,p,n);
    assert(!ble_hci_send(1,b,(uint16_t)(n+3u)));
    for(i=0;i<64 && count==reply_count;i++) ble_worker();
    assert(reply_count==count+1 && ble_get16(reply+3)==op); return reply[5];
}
static void masks(int enhanced)
{
    uint8_t mask[8]={0x10,0,0,0,0,0,0,0x20},le[8]={31,0,0,0,0,0,0,0};
    le[1]=enhanced ? 2 : 0;
    assert(!hci(0x0c01,mask,8)); assert(!hci(0x2001,le,8));
}
static void init(void)
{
    sim_reset(); sim_storage_reset(); sim_seed(); ble_init(pub,receive); connection_n=0;
    masks(0);
}
static void add(uint8_t b[39],unsigned id)
{
    memset(b,0,39); b[1]=(uint8_t)id; memcpy(b+7,irk,16); b[7]^=(uint8_t)(id-1u); b[23]=1;
}
static ble_conn_t from_rpa(void)
{
    uint8_t p[34]={0}; ble_conn_t c;
    memcpy(p,rpa,6); memcpy(p+6,pub,6); ble_put32(p+12,0x12345678); p[16]=1;
    p[19]=1; ble_put16(p+22,24); ble_put16(p+26,500); memset(p+28,255,4); p[32]=31; p[33]=5;
    assert(ble_connect_decode(&c,p,34,0x45)); return c;
}
static void connect_report(ble_conn_t *c)
{
    unsigned i; connection_n=connection_events=0; assert(ble_ll_create(c,sim_ticks));
    for(i=0;i<8 && !connection_n;i++) ble_worker();
    assert(connection_n);
    for(i=0;i<8;i++) ble_worker();
    assert(connection_events==1); /* Never both legacy and Enhanced. */
}
static void end_link(void)
{
    ble_ll_close(0x13); ble_ll_worker(); ble_worker(); assert(ble_link.state==BLE_LINK_IDLE);
}
static void accept_before_lock(void) { ble_conn_t c=from_rpa(); assert(ble_ll_create(&c,sim_ticks)); }
static void privacy(void)
{
    uint8_t b[39]={0},mode[8]={0,1,0,0,0,0,0,1},enable=1,off=0,bad=2;
    unsigned i; ble_privacy_diag_t d; ble_conn_t c;
    init(); assert(ble_ah_selftest());
    {
        uint8_t in[16]={0x94,0x81,0x70},out[16];
        static const uint8_t answer[16]={0xaa,0xfb,0x0d,0xc4,0xdc,0x1b,0x8c,0xa4,0x11,0x23,0xbe,0x2e,0xb7,0x5f,0x9d,0x15};
        assert(ble_aes_le(irk,in,out) && !memcmp(out,answer,16));
        sim_aes_fault=1; assert(!ble_ah_selftest() && !ble_crypto_selftest()); sim_aes_fault=0;
    }
    assert(!hci(0x202a,0,0) && reply[6]==BLE_RL_CAPACITY);
    assert(!hci(0x1002,0,0));
    for(i=29;i<64;i++) assert(reply[6+i]==(i==34 ? 0x78 : i==35 ? 2 : i==39 ? 4 : 0));
    assert(hci(0x2027,b,38)==0x12 && hci(0x2028,b,6)==0x12 && hci(0x2029,b,1)==0x12);
    assert(hci(0x202a,b,1)==0x12 && hci(0x202d,0,0)==0x12 && hci(0x204e,b,7)==0x12);
    for(i=1;i<=BLE_RL_CAPACITY;i++) { add(b,i); assert(!hci(0x2027,b,39)); }
    add(b,BLE_RL_CAPACITY+1u); assert(hci(0x2027,b,39)==7);
    add(b,1); assert(hci(0x2027,b,39)==0x12); /* duplicate before full */
    b[1]=99; assert(hci(0x2027,b,39)==0x12); /* duplicate non-zero IRK */
    assert(hci(0x2028,b,7)==2 && hci(0x204e,(uint8_t[8]){0,99,0,0,0,0,0,1},8)==2);
    assert(hci(0x202d,&bad,1)==0x12);
    assert(hci(0x204e,(uint8_t[8]){0,1,0,0,0,0,0,2},8)==0x12);
    b[0]=2; assert(hci(0x2027,b,39)==0x12);
    add(b,1); b[0]=1; memcpy(b+1,rpa,6); assert(hci(0x2027,b,39)==0x12);
    assert(!hci(0x2029,0,0)); add(b,1); assert(!hci(0x2027,b,39));
    assert(!hci(0x204e,mode,8)); assert(!hci(0x202d,&enable,1));
    c=from_rpa(); connect_report(&c);
    assert(connection_n==21 && connection[2]==1 && connection[7]==1 && !memcmp(connection+8,rpa,6));
    assert(!memcmp(ble_link.conn.peer,rpa,6)); end_link();
    masks(1); connect_report(&c);
    assert(connection_n==33 && connection[2]==10 && connection[7]==2 && connection[8]==1);
    assert(!memcmp(connection+20,rpa,6)); for(i=14;i<20;i++) assert(!connection[i]);
    assert(ble_get16(connection+26)==24 && ble_get16(connection+30)==500); end_link();
    assert(!hci(0x202d,&off,1)); connect_report(&c);
    assert(connection_n==33 && connection[2]==10 && connection[7]==1 && !memcmp(connection+8,rpa,6));
    for(i=14;i<26;i++) assert(!connection[i]);
    end_link();
    assert(!hci(0x202d,&enable,1)); c.peer[0]^=1; connect_report(&c);
    assert(connection_n==33 && connection[7]==1 && !memcmp(connection+8,c.peer,6));
    for(i=14;i<26;i++) assert(!connection[i]);
    end_link(); c=sim_connection();
    connect_report(&c); assert(connection[7]==0 && connection[8]==1); end_link();
    mode[7]=0; assert(!hci(0x204e,mode,8)); assert(!ble_ll_create(&c,sim_ticks));
    assert(ble_link.state==BLE_LINK_IDLE); ble_privacy_get_diag(&d); assert(d.network_rejected==1);
    assert(!hci(0x202d,&off,1)); connect_report(&c); end_link();
    assert(!hci(0x202d,&enable,1)); c=from_rpa(); sim_aes_fault=1; connect_report(&c);
    assert(connection[7]==1); sim_aes_fault=0; end_link();
    ble_privacy_get_diag(&d); assert(d.matches==2 && d.aes_failures==1 && d.unresolved==2);
    /* Enhanced alone (legacy bit 0 clear); the outer LE Meta mask gates
     * both forms and never causes a fallback legacy event. */
    { uint8_t le[8]={0,2},outer[8]={0};
      c=from_rpa(); assert(!hci(0x2001,le,8)); connect_report(&c);
      assert(connection_n==33 && connection[2]==10 && connection[7]==2); end_link();
      assert(!hci(0x0c01,outer,8)); connection_n=0; assert(ble_ll_create(&c,sim_ticks));
      for(i=0;i<8;i++) ble_worker();
      assert(!connection_n && !ble_link.created_pending); end_link(); masks(0); }
    assert(!hci(0x200a,&enable,1));
    b[0]=2; assert(hci(0x2027,b,39)==0x12); /* parameter error before state */
    add(b,2); assert(hci(0x2027,b,39)==0x0c && hci(0x2028,b,7)==0x0c);
    assert(hci(0x2029,0,0)==0x0c && hci(0x202d,&off,1)==0x0c && hci(0x204e,mode,8)==0x0c);
    assert(!hci(0x0c03,0,0)); ble_privacy_get_diag(&d); assert(!d.count && !d.enabled && !d.matches);
    masks(0); c=from_rpa(); connect_report(&c); assert(connection[7]==1 && !memcmp(connection+8,rpa,6)); end_link();
    /* Disabled resolution permits list mutation during advertising, except 202D. */
    assert(!hci(0x200a,&enable,1)); add(b,1); assert(!hci(0x2027,b,39)); assert(!hci(0x204e,mode,8));
    assert(hci(0x202d,&enable,1)==0x0c); assert(!hci(0x200a,&off,1)); assert(!hci(0x202d,&enable,1));
    c=from_rpa(); assert(ble_ll_create(&c,sim_ticks));
    assert(ble_privacy_command(0x2029,0,0)==0x0c && ble_privacy_command(0x202d,&off,0)==0x0c);
    assert(ble_privacy_command(0x2027,b,0)==0x0c && ble_privacy_command(0x2028,b,0)==0x0c &&
        ble_privacy_command(0x204e,mode,0)==0x0c);
    for(i=0;i<8;i++) ble_worker();
    end_link();
    assert(!hci(0x2028,b,7)); assert(!hci(0x2029,0,0));
    /* Zero IRK local entries coexist and never match. Random identity Enhanced type 3. */
    add(b,1); b[0]=1; b[6]=0xc1; assert(!hci(0x2027,b,39));
    mode[0]=1; mode[6]=0xc1; mode[7]=1; assert(!hci(0x204e,mode,8));
    masks(1); c=from_rpa(); connect_report(&c); assert(connection[7]==3 && connection[13]==0xc1); end_link();
    masks(0); connect_report(&c);
    assert(connection_n==21 && connection[7]==1 && !memcmp(connection+8,rpa,6)); end_link(); masks(1);
    memset(b,0,sizeof b); assert(!hci(0x2027,b,39)); b[1]=2; assert(!hci(0x2027,b,39));
    assert(hci(0x202b,0,0)==1 && hci(0x202c,0,0)==1 && hci(0x202e,0,0)==1);
    assert(!hci(0x2029,0,0));
    for(i=2;i<=4;i++) { add(b,i); assert(!hci(0x2027,b,39)); }
    add(b,1); assert(!hci(0x2027,b,39)); mode[0]=0; mode[6]=0; mode[7]=1;
    assert(!hci(0x204e,mode,8)); c=from_rpa(); connection_n=0; assert(ble_ll_create(&c,sim_ticks));
    for(i=0;i<3;i++) { ble_worker(); assert(!connection_n && ble_link.created_pending); }
    ble_worker(); assert(!connection_n); ble_worker(); assert(connection_n==33 && connection[7]==2 && connection[8]==1); end_link();
    /* Interrupt acceptance just before the poll acquires its first lock. */
    assert(!hci(0x2029,0,0)); add(b,1); b[1]=9; assert(!hci(0x2027,b,39));
    {
        uint8_t event[40],type; uint16_t n;
        sim_lock_hook=accept_before_lock;
        assert(!ble_ll_poll(&type,event,&n));
        assert(ble_ll_poll(&type,event,&n) && type==4 && event[7]==1 && !memcmp(event+8,rpa,6));
        end_link();
    }
    puts("H-22..H-25 / K-11 / F-24 connection path: PASS (legacy OTA RPA 01; Enhanced identity 02/03 plus RPA)");
}
static void store(void)
{
    union ble_store_value sec[2],cccd,got; union ble_store_key k;
    ble_bond_diag_t d; unsigned i,j,writes,erases;
    sim_reset(); sim_storage_reset(); ble_bonds_init(); memset(sec,0,sizeof sec); memset(&cccd,0,sizeof cccd);
    for(i=0;i<2;i++) {
        sec[i].sec.peer_addr.val[0]=(uint8_t)(i+1u); sec[i].sec.key_size=16; sec[i].sec.ltk_present=1; sec[i].sec.sc=i;
        assert(!ble_bonds_write(1,&sec[i]) && !ble_bonds_write(2,&sec[i]));
    }
    cccd.cccd.peer_addr=sec[0].sec.peer_addr; cccd.cccd.chr_val_handle=10; cccd.cccd.flags=1;
    assert(!ble_bonds_write(3,&cccd)); ble_bonds_flush(); writes=sim_programs[2];
    for(i=0;i<80;i++) {
        memset(&k,0,sizeof k); k.cccd.peer_addr=cccd.cccd.peer_addr; k.cccd.chr_val_handle=10;
        assert(!ble_bonds_delete(3,&k)); assert(!ble_bonds_write(3,&cccd)); ble_bonds_flush();
        assert(sim_programs[2]==writes);
    }
    /* No-op across the absent -> present -> absent state. */
    cccd.cccd.peer_addr=sec[1].sec.peer_addr; assert(!ble_bonds_write(3,&cccd));
    k.cccd.peer_addr=cccd.cccd.peer_addr; assert(!ble_bonds_delete(3,&k)); ble_bonds_flush(); assert(sim_programs[2]==writes);
    /* Genuine changes force at least one compaction while preserving two peers. */
    for(i=0;i<80;i++) {
        cccd.cccd.peer_addr=sec[i%2].sec.peer_addr; cccd.cccd.flags=(uint16_t)(i+2u);
        assert(!ble_bonds_write(3,&cccd)); sim_flash_idle=0; ble_bonds_flush(); assert(sim_programs[2]==writes+i);
        sim_flash_idle=1; ble_bonds_flush();
        if(sim_erases[2]) break;
    }
    assert(sim_erases[2]); ble_bonds_init();
    for(i=0;i<2;i++) for(j=1;j<=2;j++) {
        memset(&k,0,sizeof k); k.sec.peer_addr=sec[i].sec.peer_addr;
        assert(!ble_bonds_read((int)j,&k,&got) && got.sec.sc==i && got.sec.key_size==16);
    }
    /* Private identities refused in security and CCCD writes. */
    sec[0].sec.peer_addr.type=1; memcpy(sec[0].sec.peer_addr.val,rpa,6);
    assert(ble_bonds_write(1,sec)==BLE_HS_EINVAL && ble_bonds_write(2,sec)==BLE_HS_EINVAL);
    cccd.cccd.peer_addr=sec[0].sec.peer_addr; assert(ble_bonds_write(3,&cccd)==BLE_HS_EINVAL);
    ble_bonds_get_diag(&d); assert(d.private_refused==3);
    /* NimBLE's identity address tags normalize to the same durable key. */
    sec[1].sec.peer_addr.type=2; writes=sim_programs[2];
    assert(!ble_bonds_write(1,sec+1)); ble_bonds_flush(); assert(sim_programs[2]==writes);
    /* An old valid-CRC RPA record is dropped on load and tombstoned safely. */
    sim_storage_reset();
    { uint8_t *p=sim_flash+8192; memset(p,0,128); memcpy(p,"FLBD",4); p[4]=1; p[5]=3; p[7]=1;
      p[12]=1; memcpy(p+13,rpa,6); ble_put32(p+124,ble_crc32(p,124)); }
    ble_bonds_init(); ble_bonds_get_diag(&d); assert(!d.records && d.private_dropped==1 && d.pending);
    ble_bonds_flush(); ble_bonds_init(); ble_bonds_get_diag(&d); assert(!d.records && !d.pending);
    /* Orphan may be staged before pairing completes; compaction must drop it. */
    sim_storage_reset(); ble_bonds_init(); cccd.cccd.peer_addr.type=0; memset(cccd.cccd.peer_addr.val,0,6); cccd.cccd.peer_addr.val[0]=9;
    assert(!ble_bonds_write(3,&cccd)); ble_bonds_flush();
    memset(sec,0,sizeof sec); sec[0].sec.peer_addr.val[0]=1; sec[0].sec.key_size=16; sec[0].sec.ltk_present=1;
    for(i=0;i<40;i++) { sec[0].sec.ltk[0]=(uint8_t)i; assert(!ble_bonds_write(1,sec)); ble_bonds_flush(); }
    ble_bonds_get_diag(&d); assert(d.orphan_dropped==1 && d.records==1 && d.erases==1);
    ble_bonds_init(); memset(&k,0,sizeof k); k.cccd.peer_addr=cccd.cccd.peer_addr;
    assert(ble_bonds_read(3,&k,&got)==BLE_HS_ENOENT);
    for(i=3840;i<4096;i++) assert(sim_flash[8192+i]==255);
    /* Failed readback does not establish a durable baseline; retry uses a new slot. */
    writes=sim_programs[2]; erases=sim_erases[2]; sec[0].sec.ltk[0]=200; assert(!ble_bonds_write(1,sec));
    sim_flash_mismatch=1; ble_bonds_flush(); ble_bonds_get_diag(&d); assert(d.pending && d.failures);
    sim_flash_mismatch=0; ble_bonds_flush(); assert(sim_programs[2]==writes+2 && sim_erases[2]==erases);
    ble_bonds_init(); k.sec.peer_addr=sec[0].sec.peer_addr; assert(!ble_bonds_read(1,&k,&got) && got.sec.ltk[0]==200);
    puts("F-25 NOR simulation: two bonds, net churn/no writes, compaction/reload, orphan drop, RPA refusal/migration, failed-write retry: PASS");
}
static void timing(void)
{
    ble_timing_diag_t d; uint32_t start,item,before;
    ble_time_reset(); sim_ticks=0xfffffff0u;
    ble_link.state=BLE_LINK_UP;
    ble_time_dispatch(0xffffffe0u,0x20); ble_time_dispatch(100,99); ble_time_get_diag(&d);
    assert(d.dispatch_max==64 && d.dispatch_live_max==64 && d.dispatch_negative==1);
    ble_link.state=BLE_LINK_IDLE; ble_time_dispatch(0,200); ble_time_get_diag(&d);
    assert(d.dispatch_max==200 && d.dispatch_live_max==64);
    start=sim_ticks; ble_time_pass_begin(start); item=ble_time_item_begin(); sim_ticks+=24u*20u;
    ble_time_preempt_enter(); sim_ticks+=24u*100u; ble_time_preempt_enter(); sim_ticks+=24u*200u;
    ble_time_preempt_leave(); sim_ticks+=24u*300u; ble_time_preempt_leave(); sim_ticks+=24u*30u;
    ble_time_item_end(item,BLE_WORK_CONTROL,0); assert(ble_time_budget()); ble_time_pass_end(); ble_time_get_diag(&d);
    assert(d.wall_max==650u*24u && d.own_max==50u*24u && d.item_max==50u*24u);
    ble_time_pass_begin(sim_ticks);
    for(unsigned i=0;i<3;i++) { item=ble_time_item_begin(); sim_ticks+=90u*24u; ble_time_item_end(item,BLE_WORK_CONTROL,0); }
    assert(!ble_time_budget()); ble_time_pass_end(); ble_time_get_diag(&d);
    assert(d.own_max==270u*24u && !d.pass_over_budget && d.own_over_250us==1 && !d.item_over_100us);
    ble_time_pass_begin(sim_ticks); item=ble_time_item_begin(); sim_ticks+=101u*24u; ble_time_item_end(item,BLE_WORK_CONTROL,0); ble_time_pass_end();
    ble_time_get_diag(&d); assert(d.item_over_100us==1 && d.item_max==101u*24u);
    assert(d.item_kind==BLE_WORK_CONTROL && d.kind_max[BLE_WORK_CONTROL]==101u*24u &&
        d.kind_over_100us[BLE_WORK_CONTROL]==1 && d.kind_count[BLE_WORK_CONTROL]==5);
    ble_time_pass_begin(sim_ticks); item=ble_time_item_begin(); sim_ticks+=120u*24u;
    ble_time_item_end(item,BLE_WORK_COMMAND,0x2017); ble_time_pass_end(); ble_time_get_diag(&d);
    assert(d.item_kind==BLE_WORK_COMMAND && d.item_opcode==0x2017 &&
        d.kind_opcode[BLE_WORK_COMMAND]==0x2017 && d.kind_max[BLE_WORK_CONTROL]==101u*24u &&
        d.kind_over_100us[BLE_WORK_COMMAND]==1 && d.kind_count[BLE_WORK_COMMAND]==1);
    assert(!strcmp(ble_time_kind_name(BLE_WORK_RESOLUTION),"resolution"));
    start=sim_ticks; before=ble_time_ccm_total(); sim_ticks+=63u*24u; ble_time_ccm_add(40u*24u); ble_time_refill_end(start,before);
    ble_time_get_diag(&d); assert(d.refill_max==63u*24u && d.refill_ccm_max==40u*24u && d.refill_other_max==23u*24u && !d.refill_over_80us);
    start=sim_ticks; before=ble_time_ccm_total(); sim_ticks+=81u*24u; ble_time_ccm_add(52u*24u); ble_time_refill_end(start,before);
    ble_time_get_diag(&d); assert(d.refill_over_80us==1 && d.refill_other_max==29u*24u);
    { uint8_t a[16],b[16],changed[6]; ble_host_local_irk(pub,a); ble_host_local_irk(pub,b); assert(!memcmp(a,b,16) && (a[0]&1));
      memcpy(changed,pub,6); changed[0]^=1; ble_host_local_irk(changed,b); assert(memcmp(a,b,16)); }
    puts("F-27 arithmetic: signed dispatch/wrap/error, nested preemption counted once, own budget/item, split refill, stable nonzero local IRK: PASS");
}
static void resumable_random(void)
{
    uint8_t want[3][32],got[32],scratch[32]; unsigned i,j; ble_timing_diag_t d;
    sim_reset(); sim_storage_reset(); sim_seed();
    assert(ble_entropy(want[0],8)); ble_entropy_connection(); assert(ble_entropy(want[1],12));
    assert(ble_entropy(want[2],32));
    sim_reset(); sim_storage_reset(); sim_seed(); ble_time_reset();
    /* Deliberately injected 60 us per compression is a cost MODEL, not a
     * measured FM-1 time. Link-time wrappers count actual TinyCrypt blocks. */
    sim_sha_cost_ticks=60u*24u;
    for(i=0;i<3;i++) {
        unsigned n=i==0 ? 8 : i==1 ? 12 : 32,steps=0;
        if(i==1) ble_entropy_connection();
        assert(!ble_entropy_request(BLE_RANDOM_HCI,got,n));
        assert(!ble_entropy_request(BLE_RANDOM_LINK,scratch,12));
        while(!ble_entropy_request(BLE_RANDOM_HCI,got,n)) {
            uint32_t count=sim_sha_compressions; ble_time_pass_begin(sim_ticks);
            ble_entropy_step(); ble_time_pass_end();
            assert(sim_sha_compressions-count==1 && ++steps<8);
        }
        assert(steps==(i==1 ? 5u : 3u) && !memcmp(got,want[i],n));
    }
    ble_time_get_diag(&d);
    assert(d.item_max==60u*24u && !d.item_over_100us && !d.pass_over_budget);
    assert(d.kind_count[BLE_WORK_ENTROPY_RESEED]==2 && d.kind_count[BLE_WORK_ENTROPY_OUTPUT]==3 &&
        d.kind_count[BLE_WORK_ENTROPY_REKEY]==6 && d.kind_opcode[BLE_WORK_ENTROPY_OUTPUT]==0x2018);
    /* Cancel an abandoned link job after output generation; rekey completes,
     * then HCI can acquire the generator without a reused output/counter. */
    assert(!ble_entropy_request(BLE_RANDOM_LINK,got,12)); ble_entropy_step();
    ble_ll_reset();
    for(j=0;j<3;j++) ble_entropy_step();
    assert(!ble_entropy_request(BLE_RANDOM_HCI,got,8));
    for(j=0;j<3;j++) ble_entropy_step();
    assert(ble_entropy_request(BLE_RANDOM_HCI,got,8)==1);
    sim_sha_cost_ticks=0; ble_time_reset(); ble_time_get_diag(&d);
    assert(!d.item_max && !d.item_over_100us && !d.kind_count[BLE_WORK_ENTROPY_OUTPUT]);
    printf("F-27 resumable RNG: synchronous/async byte equality; normal 3, reseed 5 SHA blocks over separate items; model 60 us/block, per-kind max 60 us; owner exclusion/cancel/rekey: PASS\n");
}
int main(void) { privacy(); store(); timing(); resumable_random(); return 0; }
