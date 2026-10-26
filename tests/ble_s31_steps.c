/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* Actual controller/crypto with explicit pessimistic virtual-clock costs.
 * These scheduling bounds are not target measurements. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ble_s3_sim.h"
#include "ble_crypto.h"
#include "ble_privacy.h"
#include "ble_seed.h"
#include "ble_security.h"
#include "ble_timing.h"
static const uint8_t pub[6]={0x58,0x65,0x67,0x8b,0x40,0x62};
static uint8_t events[128][80],types[128],sk[16],iv[8],ltk[16];
static unsigned event_count;
static void receive(uint8_t type,const uint8_t *p,uint16_t n)
{
    assert(event_count<128 && n<=80);
    types[event_count]=type; memcpy(events[event_count++],p,n);
}
static void worker(void)
{
    ble_time_pass_begin(sim_ticks); ble_worker(); ble_time_pass_end();
}
static void ll(void)
{
    ble_time_pass_begin(sim_ticks); ble_ll_worker(); ble_time_pass_end();
}
static void one_ll(void)
{
    /* Leave one tick of this pass: the next nonzero-cost item must yield. */
    ble_time_pass_begin(sim_ticks); sim_ticks+=250u*24u-1u;
    ble_ll_worker(); ble_time_pass_end();
}
static void enqueue(uint16_t op,const uint8_t *p,unsigned n)
{
    uint8_t b[80]; ble_put16(b,op); b[2]=(uint8_t)n;
    if(n) memcpy(b+3,p,n);
    assert(!ble_hci_send(1,b,(uint16_t)(n+3u)));
}
static void command(uint16_t op,const uint8_t *p,unsigned n)
{
    unsigned before=event_count,i; enqueue(op,p,n);
    for(i=0;i<64 && event_count==before;i++) worker();
    assert(event_count==before+1 && types[before]==4 && events[before][0]==14 &&
        ble_get16(events[before]+3)==op && !events[before][5]);
}
static void init(void)
{
    uint8_t mask[8]; sim_aes_cost_ticks=sim_sha_cost_ticks=sim_memory_cost_ticks=0;
    sim_reset(); sim_storage_reset(); sim_seed(); ble_init(pub,receive); event_count=0;
    memset(mask,255,8); command(0x0c01,mask,8); command(0x2001,mask,8);
    for(unsigned i=0;i<16;i++) ltk[i]=(uint8_t)i;
    ble_time_reset();
}
static int slot(uint8_t op)
{
    for(unsigned i=0;i<2;i++) if(sim_slot[i]>=0 && ble_ll_tx_llid((unsigned)sim_slot[i])==3 &&
        ble_ll_tx_data((unsigned)sim_slot[i])[0]==op) return (int)i;
    return -1;
}
static void request(void)
{
    uint8_t p[23]={3};
    for(unsigned i=1;i<23;i++) p[i]=(uint8_t)i;
    assert(ble_ll_rx(1,0x1703,p,sim_ticks,0));
}
static void connect(void)
{
    ble_conn_t c=sim_connection(); assert(ble_ll_create(&c,sim_ticks));
    for(unsigned i=0;i<8 && ble_link.created_pending;i++) worker();
    assert(!ble_link.created_pending);
}
static void response(void)
{
    int s; unsigned i;
    for(i=0;i<16 && slot(4)<0;i++) { ble_entropy_step(); ll(); }
    s=slot(4); assert(s>=0);
    { uint8_t skd[16],key[16];
      for(i=0;i<8;i++) skd[i]=(uint8_t)(11u+i);
      memcpy(skd+8,sim_air[s]+1,8); for(i=0;i<4;i++) iv[i]=(uint8_t)(19u+i);
      memcpy(iv+4,sim_air[s]+9,4); assert(ble_aes_le(ltk,skd,key));
      for(i=0;i<16;i++) sk[i]=key[15u-i]; }
    /* LTK is not accepted before the request has been issued. */
    sim_ack((unsigned)s);
}
static void key_reply(void)
{
    unsigned before=event_count; uint8_t b[18]; worker();
    assert(event_count==before+1 && events[before][0]==0x3e && events[before][2]==5);
    ble_put16(b,BLE_HANDLE); memcpy(b+2,ltk,16); command(0x201a,b,18);
    /* The command completes before the derived key / START_ENC_REQ step. */
    assert(slot(5)<0);
}
static void start_marker(void)
{
    int s; ll(); s=slot(5); assert(s>=0); sim_ack((unsigned)s); ll();
    { ble_security_diag_t d; ble_security_get_diag(&d); assert(d.phase==BLE_ENC_S2 && d.rx_on && !d.tx_on); }
}
static void encrypted_rsp(void)
{
    uint8_t p[5]={6}; assert(ble_ccm(sk,iv,0,1,3,p,1,0));
    assert(ble_ll_rx(1,0x503,p,sim_ticks,0));
}
static void acl(void)
{
    uint8_t p[31]={0x50,0x20,27,0};
    for(unsigned i=4;i<31;i++) p[i]=(uint8_t)i;
    assert(!ble_hci_send(2,p,sizeof p));
}
static void reset_now(const char *name)
{
    unsigned before,steps=0; uint32_t epoch=ble_link.epoch;
    ble_link_diag_t link; ble_security_diag_t sec; ble_privacy_diag_t privacy;
    ble_seed_diag_t seed_before,seed_after; ble_timing_diag_t d;
    unsigned programs[3],erases[3];
    sim_aes_cost_ticks=sim_sha_cost_ticks=0; sim_memory_cost_ticks=3;
    memcpy(programs,sim_programs,sizeof programs); memcpy(erases,sim_erases,sizeof erases);
    ble_seed_get_diag(&seed_before);
    enqueue(0x0c03,0,0); worker(); before=event_count;
    assert(ble_link.state==BLE_LINK_CLOSING);
    sim_stop_result=0;
    { int held[2]={sim_slot[0],sim_slot[1]};
      for(unsigned i=0;i<3;i++) {
          ll(); worker(); assert(event_count==before);
          assert(sim_slot[0]==held[0] && sim_slot[1]==held[1]);
      } }
    /* Loaded DMA buffers are retained while stop is busy; no completion yet. */
    sim_stop_result=1;
    while(event_count==before && steps++<64) {
        uint32_t start=sim_ticks; ll(); worker();
        if(sim_ticks-start>70u*24u) fprintf(stderr,"reset cost %s step %u ticks %u\n",name,steps,sim_ticks-start);
        assert(sim_ticks-start<=70u*24u);
        assert(!ble_ll_rx_marker(BLE_MARK_RX_ON));
        ble_ll_event(0xffffu,sim_ticks);
    }
    assert(steps>20 && steps<64 && event_count==before+1);
    assert(events[before][0]==14 && ble_get16(events[before]+3)==0x0c03 && !events[before][5]);
    ble_time_get_diag(&d);
    assert(d.kind_count[BLE_WORK_RESET_CLEAR]>20 && d.kind_count[BLE_WORK_RESET_DEFAULTS]==1 &&
        d.kind_count[BLE_WORK_RESET_STATS]==1 && d.kind_count[BLE_WORK_RESET_COMPLETE]==1);
    assert(d.item_max<=70u*24u && !d.item_over_100us && !d.pass_over_budget);
    assert(d.kind_opcode[BLE_WORK_RESET_CLEAR]==0x0c03);
    ble_ll_get_diag(&link); ble_security_get_diag(&sec); ble_privacy_get_diag(&privacy);
    assert(link.state==BLE_LINK_IDLE && link.epoch!=epoch && !link.tx_used && !link.rx_used &&
        !link.ncp && !link.created && !ble_link.completed && !ble_link.instant_kind);
    assert(!sec.phase && !sec.rx_on && !sec.tx_on && !sec.rx_counter && !sec.tx_counter);
    assert(!privacy.count && !privacy.enabled);
    assert(!memcmp(programs,sim_programs,sizeof programs) && !memcmp(erases,sim_erases,sizeof erases));
    ble_seed_get_diag(&seed_after);
    assert(seed_before.state==seed_after.state && seed_before.sequence==seed_after.sequence);
    for(unsigned i=0;i<32;i++) { ll(); worker(); }
    assert(event_count==before+1); /* no old creation/LTK/enc/NCP/DC/timer event */
    sim_memory_cost_ticks=0;
    printf("r7 Reset %s: %u resumed calls, modeled item max %u ticks, clear %u, defaults %u, stats %u, complete %u; no stale events: PASS\n",
        name,steps,d.item_max,d.kind_max[BLE_WORK_RESET_CLEAR],d.kind_max[BLE_WORK_RESET_DEFAULTS],
        d.kind_max[BLE_WORK_RESET_STATS],d.kind_max[BLE_WORK_RESET_COMPLETE]);
    command(0x1009,0,0); assert(!memcmp(events[event_count-1]+6,pub,6));
    command(0x2018,0,0); /* discarded random jobs finish rekey; never stall */
}
static void resets(void)
{
    static const char *names[]={"queued ENC_REQ","decoded ENC_REQ","entropy requested","entropy mid-reseed",
        "LTK event pending","LTK delivered","LTK reply pending","START_ENC_REQ loaded",
        "RX_ON marker pending","decoded START_ENC_RSP","response/refill pending"};
    for(unsigned stage=0;stage<sizeof names/sizeof names[0];stage++) {
        init(); connect(); sim_memory_cost_ticks=1; request();
        if(stage>=1) one_ll();
        if(stage>=2) one_ll();
        if(stage>=3) { ll(); ble_entropy_step(); }
        if(stage>=4) response();
        if(stage==5) worker();
        if(stage>=6) key_reply();
        if(stage>=7) { ll(); assert(slot(5)>=0); }
        if(stage>=8) { sim_ack((unsigned)slot(5)); }
        if(stage>=9) { ll(); encrypted_rsp(); one_ll(); }
        if(stage>=10) one_ll();
        reset_now(names[stage]);
    }
    init(); connect(); request(); ll(); ll();
    for(unsigned i=0;i<5;i++) ble_entropy_step();
    assert(slot(4)<0); reset_now("entropy output ready before consumption");
    init(); sim_start_steps=3; enqueue(0x200a,(uint8_t[]){1},1); worker();
    assert(!sim_starts); enqueue(0x0c03,0,0);
    for(unsigned i=0;i<4;i++) worker();
    /* Advertising enable finishes first; Reset itself remains resumable. */
    assert(sim_starts==1 && events[event_count-1][0]==14 && ble_get16(events[event_count-1]+3)==0x200a);
    for(unsigned i=0;i<64 && ble_get16(events[event_count-1]+3)!=0x0c03;i++) worker();
    assert(ble_get16(events[event_count-1]+3)==0x0c03 && !events[event_count-1][5]);
    init(); command(0x200a,(uint8_t[]){1},1); ble_set_power(3); reset_now("active advertising");
    command(0x200a,(uint8_t[]){1},1);
    assert(sim_advertising.power==3 && sim_advertising.params.min==2048 && !sim_advertising.data_len);
    init();
    for(unsigned i=0;i<4;i++) { uint8_t b[39]={0}; b[1]=(uint8_t)(i+1); b[7]=(uint8_t)(i+1); command(0x2027,b,39); }
    command(0x202d,(uint8_t[]){1},1);
    { ble_conn_t c=sim_connection(); c.peer_type=1; memset(c.peer,0x55,6); c.peer[5]=0x40;
      assert(ble_ll_create(&c,sim_ticks)); worker(); assert(ble_link.resolve_index==1 && !ble_link.resolve_done); }
    reset_now("mid-resolution");
    init(); connect();
    { unsigned before=event_count; uint8_t p[18]={0x50};
      enqueue(0x201a,p,18); enqueue(0x2001,(uint8_t[8]){0},8); enqueue(0x0c03,0,0);
      for(unsigned i=0;i<64 && event_count<before+3;i++) worker();
      assert(event_count==before+3 && events[before][5]==0x0c && events[before+1][5]==0x0c &&
          ble_get16(events[before+2]+3)==0x0c03 && !events[before+2][5]); }
    puts("r7 Reset: mid-advertising publication FIFO, power/defaults/public/DRBG persistence, pending-resolution cancellation and queued-reply ordering: PASS");
}
static void split_control(void)
{
    ble_security_diag_t d; ble_timing_diag_t timing; int s; uint32_t blocks;
    init(); connect(); request(); response(); key_reply(); start_marker();
    acl(); acl(); assert(sim_slot[0]<0 && sim_slot[1]<0); /* barrier */
    encrypted_rsp(); ble_time_reset(); sim_aes_cost_ticks=8u*24u; sim_memory_cost_ticks=1;
    blocks=sim_aes_blocks; one_ll(); assert(sim_aes_blocks-blocks==5); ble_security_get_diag(&d);
    assert(d.phase==BLE_ENC_S2 && d.rx_counter==1 && !d.tx_on && slot(6)<0);
    /* Stream cannot pass the authenticated, pending control before dispatch. */
    blocks=sim_aes_blocks; one_ll(); assert(sim_aes_blocks==blocks); ble_security_get_diag(&d);
    assert(d.encrypted && d.tx_on && !d.tx_counter && slot(6)<0);
#if FM1B_BLE_RELIABILITY
    /* R7 precomputes ciphertext without committing a counter or choosing a
     * DMA slot, then checks hardware in the normal one-slot refill. */
    blocks=sim_aes_blocks; one_ll(); assert(sim_aes_blocks==blocks && slot(6)<0);
    for(unsigned i=0;i<64 && slot(6)<0;i++) one_ll();
    s=slot(6); assert(s>=0); ble_security_get_diag(&d); assert(d.tx_counter==1);
    for(unsigned i=0;i<64 && (sim_slot[0]<0 || sim_slot[1]<0);i++) one_ll();
    ble_security_get_diag(&d); assert(d.tx_counter==2 && sim_slot[0]>=0 && sim_slot[1]>=0);
#else
    blocks=sim_aes_blocks; one_ll(); assert(sim_aes_blocks-blocks==5);
    s=slot(6); assert(s>=0); ble_security_get_diag(&d); assert(d.tx_counter==1);
    { unsigned other=(unsigned)s^1u; assert(sim_slot[other]<0); }
    blocks=sim_aes_blocks; one_ll(); assert(sim_aes_blocks-blocks==7);
    ble_security_get_diag(&d); assert(d.tx_counter==2 && sim_slot[0]>=0 && sim_slot[1]>=0);
#endif
    ble_time_get_diag(&timing);
    assert(timing.kind_count[BLE_WORK_CONTROL_RX]==1 && timing.kind_count[BLE_WORK_CONTROL]==1 &&
        timing.kind_count[BLE_WORK_REFILL]
#if FM1B_BLE_RELIABILITY
        >=2);
#else
        ==2);
#endif
    assert(timing.kind_opcode[BLE_WORK_CONTROL_RX]==6 && timing.kind_opcode[BLE_WORK_CONTROL]==6 &&
        timing.kind_opcode[BLE_WORK_REFILL]==6);
    assert(timing.item_max<=70u*24u && !timing.item_over_100us && !timing.pass_over_budget);
    printf("r7 control opcode 06: AES 8 us/block + controller memory 1 tick/byte MODEL; control_rx %u, control %u, one-slot refill %u ticks; each <=70 us: PASS\n",
        timing.kind_max[BLE_WORK_CONTROL_RX],timing.kind_max[BLE_WORK_CONTROL],timing.kind_max[BLE_WORK_REFILL]);
    sim_aes_cost_ticks=sim_memory_cost_ticks=0;
    { uint8_t p[5]; memcpy(p,sim_air[s],5); assert(ble_ccm(sk,iv,0,0,3,p,1,1) && p[0]==6); }
    /* Pause: decrypt and barrier occur in stream order; ack marker before clear RSP. */
    sim_ack(0); sim_ack(1);
    { uint8_t p[5]={10}; assert(ble_ccm(sk,iv,1,1,3,p,1,0)); assert(ble_ll_rx(1,0x503,p,sim_ticks,0)); }
    sim_aes_cost_ticks=8u*24u; sim_memory_cost_ticks=1; one_ll(); one_ll();
    ble_security_get_diag(&d);
#if FM1B_BLE_RELIABILITY
    for(unsigned i=0;i<64 && d.phase!=BLE_ENC_PA;i++) { one_ll(); ble_security_get_diag(&d); }
#endif
    assert(d.phase==BLE_ENC_PA && d.rx_on && d.tx_on);
    ll();
#if FM1B_BLE_RELIABILITY
    for(unsigned i=0;i<64 && slot(11)<0;i++) ll();
#endif
    s=slot(11); assert(s>=0); sim_ack((unsigned)s);
    assert(ble_ll_rx(1,0x103,(uint8_t[]){11},sim_ticks,0)); ll();
    ble_security_get_diag(&d); assert(d.phase==BLE_ENC_PC && !d.rx_on && !d.tx_on && !d.rx_counter && !d.tx_counter);
    sim_aes_cost_ticks=sim_memory_cost_ticks=0;
    puts("r7 split control: first encrypted response takes TX counter 0 ahead of ACL; pause RX_OFF marker and clear response remain ordered: PASS");
    init(); connect(); request(); response(); key_reply(); start_marker();
    ble_time_reset(); sim_aes_cost_ticks=8u*24u;
    assert(ble_ll_rx(1,0x503,(uint8_t[5]){0},sim_ticks,0)); ll();
#if FM1B_BLE_RELIABILITY
    for(unsigned i=0;i<256 && ble_reliability_failed();i++) ll();
#endif
    ble_time_get_diag(&timing);
    assert(ble_link.reason==0x3d && timing.kind_opcode[BLE_WORK_CONTROL_RX]==0xffffu &&
        !timing.kind_count[BLE_WORK_CONTROL]);
    sim_aes_cost_ticks=0;
    puts("r7 failed control MIC: opcode FFFF (unavailable), no dispatch/refill: PASS");
}
static void full_resolution(void)
{
    for(unsigned local=0;local<2;local++) for(unsigned hit=0;hit<2;hit++) {
        init();
        uint8_t b[39]={0}; if(local) command(0x2027,b,39); /* profile local zero IRK */
        for(unsigned i=local;i<BLE_RL_CAPACITY;i++) {
            memset(b,0,39); b[1]=(uint8_t)(i+1); b[7]=(uint8_t)(i+1); command(0x2027,b,39);
            uint8_t mode[8]={0}; mode[1]=(uint8_t)(i+1); mode[7]=1; command(0x204e,mode,8);
        }
        command(0x202d,(uint8_t[]){1},1);
        ble_conn_t c=sim_connection(); c.peer_type=1; c.peer[3]=0x94; c.peer[4]=0x81; c.peer[5]=0x70;
        assert(ble_ah(b+7,c.peer+3,c.peer)); if(!hit) c.peer[0]^=1;
        unsigned before=event_count,blocks=sim_aes_blocks;
        assert(ble_ll_create(&c,sim_ticks)); ble_time_reset();
        /* Deliberately model 2 x the HAL shared 26-us transaction deadline,
         * plus memory costs; this is pessimistic, not a target measurement. */
        sim_aes_cost_ticks=52u*24u; sim_memory_cost_ticks=1;
        for(unsigned step=0;step<BLE_RL_CAPACITY;step++) {
            uint32_t start=sim_ticks,old=sim_aes_blocks; worker();
            assert(sim_aes_blocks-old<=BLE_RL_BLOCKS_PER_ITEM && sim_ticks-start<=100u*24u);
            assert(event_count==before && ble_link.resolve_index==step+1u);
            assert(ble_link.resolve_done==(step+1u==BLE_RL_CAPACITY));
        }
        assert(sim_aes_blocks-blocks==BLE_RL_CAPACITY-local && ble_link.peer_resolved==hit);
        worker(); assert(event_count==before+1 && events[before][2]==10);
        ble_timing_diag_t d; ble_time_get_diag(&d);
        assert(d.kind_count[BLE_WORK_RESOLUTION]==BLE_RL_CAPACITY && d.kind_max[BLE_WORK_RESOLUTION]<=100u*24u && !d.item_over_100us);
        printf("r8 full RL %s: %u entries, %u AES blocks in %u items, modeled resolution max %u ticks <=2400, event deferred: PASS\n",
            hit ? "last match" : "miss",BLE_RL_CAPACITY,BLE_RL_CAPACITY-local,BLE_RL_CAPACITY,d.kind_max[BLE_WORK_RESOLUTION]);
        sim_aes_cost_ticks=sim_memory_cost_ticks=0;
    }
}
int main(void) { resets(); split_control(); full_resolution(); return 0; }
