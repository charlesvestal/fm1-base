/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* R4 independent central: production HAL RX/refill + real controller/CCM.
 * The virtual baseband implements Core stop-and-wait and r7 status/toggles.
 * No undocumented silicon latch or RF-corruption probability is simulated. */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble_mic_radio.h"
#include "../open_ble/ble_crypto.h"
#include "../open_ble/ble_security.h"
#include "../open_ble/ble_seed.h"
static uint8_t sk[16],iv[8],sn,expected_sn;
static uint64_t next_counter,accepted_counter;
static int start_pressure_mode;
static uint16_t start_interval=12;
static uint32_t rng=0x4d494334u,sequence,delivered,crc_errors,repeats,missed,ack_lost;
static uint32_t random32(void) { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static void work(void) {
#if FM1B_BLE_RELIABILITY
    for(unsigned i=0;i<256;i++) { ble_ll_worker(); if(i>=8 && !ble_reliability_pending()) break; }
#else
    for(unsigned i=0;i<8;i++) ble_ll_worker();
#endif
}
static int opcode(uint8_t op)
{
    for(unsigned i=0;i<2;i++) if(sim_slot[i]>=0 && ble_ll_tx_llid((unsigned)sim_slot[i])==3 &&
        ble_ll_tx_data((unsigned)sim_slot[i])[0]==op) return (int)i;
    return -1;
}
static void direct(const uint8_t *p,uint8_t n,uint8_t llid,uint16_t event,int encrypted)
{
    uint8_t air[255]={0}; memcpy(air,p,n);
    if(encrypted) { assert(ble_ccm(sk,iv,next_counter++,1,llid,air,n,0)); n+=4; accepted_counter++; }
    sim_ticks+=(80u+8u*n)*24u;
    assert(sim_radio_receive(1,(uint16_t)((n<<8)|llid|(sn<<3)),air,event)); sn^=1; expected_sn^=1;
    sim_radio_service(0); work();
}
static uint32_t begin(void)
{
    ble_conn_t c=sim_connection(); uint8_t req[23]={3},ltk[16],skd[16],le[16]; int slot;
    sim_reset(); sim_storage_reset(); sim_seed(); c.interval=start_interval;
    assert(ble_ll_create(&c,sim_ticks)); sim_radio_start();
    sn=expected_sn=0; next_counter=accepted_counter=0;
    sim_ticks=ble_link.time.first;
    for(unsigned i=0;i<8;i++) req[11+i]=(uint8_t)(i+33u);
    direct(req,23,3,0,0);
    for(unsigned i=0;i<16;i++) { ble_entropy_step(); work(); }
    slot=opcode(4); assert(slot>=0);
    memcpy(skd,req+11,8); memcpy(skd+8,sim_air[slot]+1,8);
    memcpy(iv,req+19,4); memcpy(iv+4,sim_air[slot]+9,4);
    for(unsigned i=0;i<16;i++) ltk[i]=(uint8_t)i;
    assert(ble_aes_le(ltk,skd,le)); for(unsigned i=0;i<16;i++) sk[i]=le[15-i];
    sim_ack((unsigned)slot); assert(!ble_ll_ltk_reply(BLE_HANDLE,ltk,0)); work();
    slot=opcode(5); assert(slot>=0);
    if(start_pressure_mode) {
        uint8_t clear[2]={7,4},air[5]={6};
        for(unsigned i=0;i<BLE_RX_BACKLOG;i++) {
            assert(sim_radio_receive(1,(uint16_t)(0x203u|(sn<<3)),clear,0)); sn^=1; expected_sn^=1;
            sim_radio_service(0); /* Keep all S1 PDUs queued before RX_ON. */
        }
        sim_ack((unsigned)slot);
        assert(ble_ccm(sk,iv,next_counter++,1,3,air,1,0)); accepted_counter++;
        assert(sim_radio_receive(1,(uint16_t)(0x503u|(sn<<3)),air,0)); sn^=1; expected_sn^=1;
        uint32_t pressure=sim_radio_pressure;
        sim_radio_service(0);
        assert(sim_radio_full()==1 && sim_radio_pressure==pressure+1u);
        work(); start_pressure_mode=0;
    } else {
    /* Older clear S1 packet and encrypted ACK carrier in both full slots. */
    { uint8_t clear[2]={7,4},air[5]={6};
      assert(sim_radio_receive(1,(uint16_t)(0x203u|(sn<<3)),clear,0)); sn^=1; expected_sn^=1;
      sim_ack((unsigned)slot);
      assert(ble_ccm(sk,iv,next_counter++,1,3,air,1,0)); accepted_counter++;
      assert(sim_radio_receive(1,(uint16_t)(0x503u|(sn<<3)),air,0)); sn^=1; expected_sn^=1;
      sim_radio_service(0); work(); }
    }
    slot=opcode(6); assert(slot>=0 && sim_air_len[slot]==5); sim_ack((unsigned)slot);
    ble_security_diag_t d; ble_security_get_diag(&d);
    assert(d.encrypted && d.rx_counter==1 && !d.mic_failures);
    uint8_t type,p[80]; uint16_t n; while(ble_ll_poll(&type,p,&n)) { }
    return ble_link.time.first;
}
typedef struct { uint8_t n,p[27]; } expected_t;
static expected_t oracle[128]; static unsigned ow,orr,oc;
static void drain(void)
{
    uint8_t p[80],type; uint16_t n;
    for(unsigned i=0;i<128;i++) {
        work();
        if(!ble_ll_poll(&type,p,&n)) { if(!ble_ll_pending()) break; continue; }
        if(type==2) {
            assert(oc && n==oracle[orr].n+4u && !memcmp(p+4,oracle[orr].p,oracle[orr].n));
            orr=(orr+1u)%128u; oc--; delivered++;
        }
    }
}
static void acknowledge_all(void)
{
    for(unsigned i=0;i<2;i++) if(sim_slot[i]>=0 && !ble_ll_tx_boundary((unsigned)sim_slot[i])) sim_ack(i);
}
static void randomized(uint32_t events)
{
    rng=0x4d494334u; /* Preserve the inherited stress stream independently of ping tests. */
    uint32_t anchor=begin(),updates=0,delay=0;
    uint16_t interval=12,new_interval=12,instant=0;
    unsigned update_active=0,pending=0,new_update=0;
    uint8_t air[31]={0},plain[27]={0},length=0,llid=1,pdu_sn=0;
    uint64_t pdu_counter=0;
    static const uint16_t cycle[]={12,24,9,12};
    for(uint32_t e=0;e<events;e++) {
        uint16_t event=(uint16_t)e;
        if(update_active && event==instant) { interval=new_interval; update_active=0; }
        unsigned near_update=update_active || (e%4096u)<20u;
        unsigned pressure=(e%1024u)>=64u && (e%1024u)<144u;
        if(near_update) { sim_ticks=anchor; sim_radio_service(1); drain(); }
        unsigned burst=(random32()%32u)==0 ? 2u+random32()%7u : 1;
        if(pressure && (e%1024u)<81u) burst=8;
        for(unsigned j=0;j<burst;j++) {
            sim_ticks=anchor+(80u+j*708u)*24u;
            if(!pending) {
                pdu_sn=sn; length=0; llid=1; new_update=0;
                if(e && e%4096u==0 && !update_active) {
                    memset(plain,0,12); plain[1]=1;
                    new_interval=cycle[(updates+1u)%4u]; ble_put16(plain+4,new_interval);
                    ble_put16(plain+8,500); instant=(uint16_t)(event+12u); ble_put16(plain+10,instant);
                    length=12; llid=3; new_update=1;
                } else if((random32()&15u)==0 || burst>1) {
                    length=(uint8_t)(1u+random32()%27u); llid=2; sequence++;
                    for(unsigned k=0;k<length;k++) plain[k]=(uint8_t)(sequence+k);
                    if((random32()&31u)==0) { plain[0]=7; plain[1]=14; length=2; llid=3; }
                }
                if(length) {
                    memcpy(air,plain,length); pdu_counter=next_counter++;
                    assert(ble_ccm(sk,iv,pdu_counter,1,llid,air,length,0)); length+=4;
                }
                pending=1;
            }
            unsigned roll=random32();
            if((roll&255u)==0 && !near_update) { missed++; continue; }
            int crc=(roll%97u)==0 && !near_update;
            unsigned cls=crc ? ((roll&256u) ? 5u : 13u) : pdu_sn==expected_sn ? 1u : 9u;
            uint16_t header=(uint16_t)((length<<8)|llid|(pdu_sn<<3)|((roll&1u)<<2)|((j+1u<burst || (roll&2u)) ? 16u : 0));
            /* End-of-packet service stamp lets the production anchor estimator
             * recover the independent central grid on the first exchange. */
            sim_ticks=anchor+(80u+8u*length+j*708u)*24u;
            int stored=sim_radio_receive((uint16_t)cls,header,air,event);
            if(stored && cls==1) {
                expected_sn^=1;
                if(length) {
                    assert(pdu_counter==accepted_counter++);
                    if(llid!=3) { assert(oc<128); oracle[ow].n=(uint8_t)(length-4u);
                        memcpy(oracle[ow].p,plain,length-4u); ow=(ow+1u)%128u; oc++; }
                    if(new_update) { update_active=1; updates++; }
                }
            }
            if(stored && cls==9) repeats++;
            if(crc) crc_errors++;
            if(stored && !crc) {
                if((random32()%41u)==0 && !near_update) { ack_lost++; }
                else { pending=0; sn^=1; }
            }
            if(near_update || (random32()%4u)) sim_radio_service(0);
        }
        sim_ticks=anchor+6000u*24u;
        if(near_update || !delay) {
            sim_radio_service(0);
            if(near_update || (!pressure && (random32()%5u))) drain(); else work();
            if(!near_update && (random32()%127u)==0) delay=1u+random32()%12u;
        } else delay--;
        acknowledge_all();
        ble_ll_event(event,sim_ticks); ble_link.rx_seen=ble_link.valid_seen=0;
        assert(ble_link.state==BLE_LINK_UP);
        if(near_update) drain();
        anchor+=(uint32_t)interval*30000u;
        if((e+1u)%1000000u==0) {
            ble_security_diag_t d; ble_security_get_diag(&d); assert(!d.mic_failures);
            printf("events=%u accepted_encrypted=%" PRIu64 " delivered=%u repeats=%u crc=%u both=%u pressure=%u refused=%u updates=%u\n",
                e+1u,accepted_counter,delivered,repeats,crc_errors,sim_radio_both,sim_radio_pressure,sim_radio_refused,updates); fflush(stdout);
        }
    }
    sim_radio_service(1); drain();
    ble_security_diag_t d; ble_security_get_diag(&d);
    assert(!oc && !d.mic_failures && d.rx_counter==accepted_counter);
    assert(repeats && crc_errors && sim_radio_both && sim_radio_refused && updates);
#if !FM1B_BLE_RELIABILITY
    assert(sim_radio_pressure);
#endif
    assert(!ble_link.diag.late_instants && !ble_link.diag.supervision_fail);
    printf("random seed=4d494334 events=%u counter=%" PRIu64 " missed=%u lost_ack=%u zero spontaneous MIC failures: PASS\n",events,d.rx_counter,missed,ack_lost);
}
#include "ble_central_ccm.inc"
#include "ble_ping_stress.inc"
#include "ble_radio_trace_test.inc"
static void corruption(void)
{
    /* Force both software tiers full and both DMA slots held. The final PDU
     * has an invalid MIC, but a simulated good CRC and accepted new SN. */
    begin(); uint64_t bad_counter=0; uint32_t epoch=ble_link.epoch;
    for(unsigned i=0;i<2;i++) {
        direct((uint8_t[]){18},1,3,1,1);
        int slot=opcode(7); assert(slot>=0); sim_ack((unsigned)slot);
    }
    ble_mic_record_t records[BLE_MIC_RECORDS];
    for(unsigned i=0;i<40;i++) {
        uint8_t p[5]={(uint8_t)i};
        bad_counter=next_counter++; assert(ble_ccm(sk,iv,bad_counter,1,2,p,1,0));
        if(i==39) p[4]^=1;
        assert(sim_radio_receive(1,(uint16_t)(0x502u|(sn<<3)|16u),p,1)); sn^=1;
        sim_radio_service(0); work();
    }
    assert(sim_radio_full()==2); sim_radio_service(1);
    for(unsigned i=0;i<256 && ble_link.state<BLE_LINK_CLOSING;i++) {
        uint8_t type,p[80]; uint16_t n; (void)ble_ll_poll(&type,p,&n); work();
    }
    assert(ble_link.reason==0x3d);
    unsigned n=ble_security_mic_records(records); assert(n);
    ble_mic_record_t saved=records[n-1];
    assert(saved.epoch==epoch && saved.counter==bad_counter && saved.interval==12 && saved.header>>8==5);
    assert(saved.slot<2 && saved.flags&BLE_RX_DELAYED && saved.flags&BLE_RX_PRESSURE && saved.event==65535);
    assert(saved.rx_count==4 && saved.rx[3].counter==bad_counter-1u);
    assert(saved.tx_count==2 && saved.tx[0].counter+1u==saved.tx[1].counter);
    work(); assert(ble_security_mic_records(records)==n && !memcmp(&saved,&records[n-1],sizeof saved));
    ble_ll_reset(); assert(ble_security_mic_records(records)==n && !memcmp(&saved,&records[n-1],sizeof saved));
    puts("corrupted MIC: 0x3d, exact counter/header/history/pressure flags, journal survives stop and HCI Reset: PASS");
}
static uint8_t racing_air[5]; static uint16_t racing_header;
static void reuse_freed_slot(void)
{
    assert(sim_radio_receive(1,racing_header,racing_air,1));
}
static void slot_race_and_lengths(void)
{
    begin(); uint8_t a[5]={21},b[5]={22}; uint64_t base=next_counter;
    assert(ble_ccm(sk,iv,next_counter++,1,2,a,1,0));
    assert(sim_radio_receive(1,(uint16_t)(0x502u|(sn<<3)),a,1)); sn^=1;
    assert(ble_ccm(sk,iv,next_counter++,1,2,b,1,0));
    assert(sim_radio_receive(1,(uint16_t)(0x502u|(sn<<3)),b,1)); sn^=1;
    racing_air[0]=23; assert(ble_ccm(sk,iv,next_counter++,1,2,racing_air,1,0));
    racing_header=(uint16_t)(0x502u|(sn<<3)); sn^=1;
    sim_radio_release_hook=reuse_freed_slot;
    sim_radio_service(0); assert(!sim_radio_release_hook && sim_radio_full()==1);
    sim_radio_service(1); work();
    for(unsigned i=0;i<3;i++) {
        uint8_t p[80],type; uint16_t n;
        assert(ble_ll_poll(&type,p,&n) && type==2 && n==5 && p[4]==21u+i);
    }
    ble_security_diag_t d; ble_security_get_diag(&d); assert(d.rx_counter==base+3u && !d.mic_failures);
    /* The documented unsupported-length class D never reads the small
     * snapshot, authenticates, or consumes a counter, even at length 255. */
    for(unsigned length=32;length<=255;length+=223) {
        uint8_t p[255]={0};
        assert(sim_radio_receive(1,(uint16_t)((length<<8)|2u),p,1));
        sim_radio_service(0); work(); ble_security_get_diag(&d); assert(d.rx_counter==base+3u);
    }
    for(unsigned cls=5;cls<=13;cls+=8) {
        uint8_t garbage[255]={0};
        assert(sim_radio_receive((uint16_t)(cls|0x100u),0xff1f,garbage,1));
        sim_radio_service(0); work(); ble_security_get_diag(&d); assert(d.rx_counter==base+3u);
    }
    unsigned serial=0;
    for(unsigned length=1;length<=4;length++) {
        begin(); uint8_t p[4]={0}; ble_mic_record_t records[BLE_MIC_RECORDS];
        assert(sim_radio_receive(1,(uint16_t)((length<<8)|3u|(sn<<3)),p,1));
        sim_radio_service(0); work(); assert(ble_link.reason==0x3d);
        unsigned count=ble_security_mic_records(records); assert(count);
        const ble_mic_record_t *r=&records[count-1u];
        assert(r->counter==1 && r->header>>8==length && r->slot<2 && !r->flags);
        assert(!serial || r->serial==serial+1u); serial=r->serial;
    }
    ble_mic_record_t records[BLE_MIC_RECORDS]; assert(ble_security_mic_records(records)==4);
    assert(records[3].serial-records[0].serial==3);
    puts("DMA reuse during both-full service preserves A/B/C FIFO; lengths 32/255 filtered, 1..4 fail once; four-record journal wraps: PASS");
}
static void pause_restart(void)
{
    start_pressure_mode=1; begin();
    puts("RX_ON reservation blocked with one full slot: wait retains delayed/pressure state, marker/PDU recover once: PASS");
    for(unsigned terminating=0;terminating<2;terminating++) {
        begin(); direct((uint8_t[]){10},1,3,1,1);
        int slot=opcode(11); assert(slot>=0);
        if(terminating) {
            uint8_t p[6]={2,0x13};
            assert(ble_ccm(sk,iv,next_counter++,1,3,p,2,0));
            assert(sim_radio_receive(1,(uint16_t)(0x603u|(sn<<3)),p,1)); sn^=1;
        }
        sim_ack((unsigned)slot);
        uint8_t clear=11;
        assert(sim_radio_receive(1,(uint16_t)(0x103u|(sn<<3)),&clear,1)); sn^=1;
        sim_radio_service(0); work();
        ble_security_diag_t d; ble_security_get_diag(&d); assert(!d.mic_failures);
        if(terminating) { assert(ble_link.peer_stop && ble_link.reason==0x13); continue; }
        assert(d.phase==BLE_ENC_PC && !d.rx_counter && !d.tx_counter && !d.rx_on && !d.tx_on);
        uint8_t req[23]={3},ltk[16],skd[16],le[16];
        for(unsigned i=0;i<8;i++) req[11+i]=(uint8_t)(i+77u);
        direct(req,23,3,1,0);
        for(unsigned i=0;i<16;i++) { ble_entropy_step(); work(); }
        slot=opcode(4); assert(slot>=0);
        memcpy(skd,req+11,8); memcpy(skd+8,sim_air[slot]+1,8);
        memcpy(iv,req+19,4); memcpy(iv+4,sim_air[slot]+9,4);
        for(unsigned i=0;i<16;i++) ltk[i]=(uint8_t)i;
        assert(ble_aes_le(ltk,skd,le)); for(unsigned i=0;i<16;i++) sk[i]=le[15-i];
        sim_ack((unsigned)slot); assert(!ble_ll_ltk_reply(BLE_HANDLE,ltk,0)); work();
        slot=opcode(5); assert(slot>=0); sim_ack((unsigned)slot);
        next_counter=accepted_counter=0; direct((uint8_t[]){6},1,3,1,1);
        ble_security_get_diag(&d); assert(d.encrypted && d.rx_counter==1 && d.tx_counter==1 && d.refreshes==1 && !d.mic_failures);
    }
    puts("production HAL RX_ON/RX_OFF: clear/encrypted coalescing, old encrypted terminate before pause marker, new key/counter-zero restart: PASS");
}
static void update_age_record(void)
{
    uint32_t anchor=begin(),update_ms=0; uint8_t update[12]={0,1,0,0,24,0,0,0,244,1,8,0};
    sim_ticks=anchor+3000u*24u; direct(update,12,3,0,1);
    sim_ticks=anchor+6000u*24u; ble_ll_event(0,sim_ticks); work();
    ble_link.rx_seen=ble_link.valid_seen=0; anchor+=12u*30000u;
    for(unsigned e=1;e<=5675;e++) {
        sim_ticks=anchor+80u*24u;
        assert(sim_radio_receive(1,(uint16_t)(1u|(sn<<3)|16u),0,(uint16_t)e)); sn^=1;
        sim_radio_service(0); work();
        if(e==8) update_ms=ble_port_ms();
        sim_ticks=anchor+1000u*24u; ble_ll_event((uint16_t)e,sim_ticks); work();
        ble_link.rx_seen=ble_link.valid_seen=0;
        anchor+=(e<8 ? 12u : 24u)*30000u;
    }
    uint8_t bad[5]={0}; uint64_t count=next_counter++;
    assert(ble_ccm(sk,iv,count,1,2,bad,1,0)); bad[1]^=1;
    sim_ticks=anchor+(80u+8u*5u)*24u;
    assert(sim_radio_receive(1,(uint16_t)(0x502u|(sn<<3)|16u),bad,5676));
    sim_radio_service(0); work(); assert(ble_link.reason==0x3d);
    ble_mic_record_t records[BLE_MIC_RECORDS]; unsigned n=ble_security_mic_records(records);
    const ble_mic_record_t *r=&records[n-1u];
    assert(r->event==5676 && r->service_event==5676 && r->interval==24 && !r->flags);
    uint32_t age=r->received_ms-update_ms;
    assert(r->since_update_ms>=170000u && r->since_update_ms<171000u);
    assert(r->since_update_ms==age || r->since_update_ms==age+1u);
    printf("MIC after update/tick wrap: event=%u interval=%u age_ms=%u exact counter=%" PRIu64 ": PASS\n",
        r->event,r->interval,r->since_update_ms,r->counter);
}
static char dump[131072]; static unsigned dump_used;
static void ob_putc(char c) { assert(dump_used+1u<sizeof dump); dump[dump_used++]=c; dump[dump_used]=0; }
static void ob_puts(const char *p) { while(*p) ob_putc(*p++); }
static void ob_dec(int32_t value) { char b[16]; snprintf(b,sizeof b,"%" PRId32,value); ob_puts(b); }
static void ob_hex(uint32_t value,unsigned n) { while(n--) ob_putc("0123456789ABCDEF"[(value>>(n*4u))&15u]); }
static void ob_kv(const char *name,int32_t value) { ob_puts(name); ob_putc(' '); ob_dec(value); ob_putc('\n'); }
#include "mic_dump_seam.inc"
#define BRD_PUTS ob_puts
#define BRD_DEC ob_dec
#define BRD_HEX ob_hex
#include "../open_ble/ble_radio_dump.inc"
static void dump_test(void)
{
    open_ble_mic_dump(); assert(strstr(dump,"mic_records 4\n") && strstr(dump,"mic_rx serial "));
    assert(strstr(dump,"service_event 5676 interval 24 since_update_ms "));
    assert(strstr(dump," llid 2 sn ") && strstr(dump," md 1 length 5 counter 0000000002"));
    unsigned line=0;
    for(const char *p=dump;*p;p++) { if(*p=='\n') line=0; else assert(++line<188u); }
    assert(!strstr(dump,"key") && !strstr(dump,"plaintext"));
    ble_radio_dump();
    assert(strstr(dump,"radio_frozen 1 reason 3D events ") && strstr(dump,"radio_tx_source descriptor_only"));
    assert(strstr(dump," decision 6 ctr 0000000002") && strstr(dump," decision 2 ctr none"));
    for(const char *p=dump;*p;p++) { if(*p=='\n') line=0; else assert(++line<188u); }
    puts("production USB/CDC MIC formatter: complete fields, 39-bit hex counters, all logical rows fit USB frame: PASS");
}
#if FM1B_BLE_RELIABILITY
#include "ble_reliability_test.inc"
#endif
int main(int argc,char **argv)
{
    #if FM1B_BLE_RELIABILITY
    if(argc>2 && !strcmp(argv[1],"--r7")) { r7_test(argv[2]); return 0; }
#endif
    if(argc>1 && !strcmp(argv[1],"--trace-timeout")) { radio_timeout_test(); return 0; }
    uint32_t events=argc>1 ? (uint32_t)strtoul(argv[1],0,0) : 10000000u;
    central_ccm_test(); ping_stress(events/2u,12); ping_stress(events/2u,24); randomized(events);
#if FM1B_BLE_RELIABILITY
    return 0;
#endif
    radio_trace_test(); corruption(); slot_race_and_lengths(); pause_restart(); update_age_record(); dump_test(); return 0;
}
