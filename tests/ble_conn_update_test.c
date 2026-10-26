/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* Independent central event grid driving the existing virtual-radio ports.
 * Port writes are observed; no simulation of undocumented anchor latches. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ble_s2_sim.h"
#if FELUCCA_BLE_STAGE >= 3
#include "ble_s3_sim.h"
#include "ble_crypto.h"
#include "ble_seed.h"
#include "ble_security.h"
#endif
static unsigned updates,disconnects;
static uint8_t last_reason;
static void host(uint8_t type,const uint8_t *p,uint16_t n) { (void)type; (void)p; (void)n; }
static void drain(void)
{
    uint8_t p[80],type; uint16_t n;
    for(unsigned i=0;i<32;i++) {
        if(!ble_ll_poll(&type,p,&n)) { if(ble_link.created_pending) continue; break; }
        if(type==4 && p[0]==0x3e && p[2]==3) { assert(!p[3]); updates++; }
        if(type==4 && p[0]==5) { disconnects++; last_reason=p[5]; }
    }
}
static void work(void) {
#if FM1B_BLE_RELIABILITY
    for(unsigned i=0;i<256;i++) { ble_ll_worker(); if(i>=4 && !ble_reliability_pending()) break; }
#else
    for(unsigned i=0;i<4;i++) ble_ll_worker();
#endif
}
static uint32_t begin(uint16_t interval,uint32_t origin)
{
    static const uint8_t pub[6]={1,2,3,4,5,6};
    ble_conn_t c=sim_connection(); sim_reset(); sim_ticks=origin;
#if FELUCCA_BLE_STAGE >= 3
    sim_storage_reset(); sim_seed();
#endif
    ble_init(pub,host); updates=disconnects=last_reason=0;
    c.interval=interval; c.offset=interval/2u;
    assert(ble_ll_create(&c,sim_ticks)); drain();
    return ble_link.time.first;
}
static void packet(uint16_t event,uint32_t anchor,const uint8_t *p,uint8_t n)
{
    sim_ticks=anchor+(80u+8u*n)*24u;
    assert(ble_ll_rx(1,(uint16_t)((uint16_t)n<<8)|(n ? 3u : 1u),p,sim_ticks,event));
    work();
}
static void close_event(uint16_t event,uint32_t anchor)
{
    sim_ticks=anchor+1000u*24u; ble_ll_event(event,sim_ticks);
    ble_link.rx_seen=ble_link.valid_seen=0; work(); drain();
}
static void encode(uint8_t p[12],uint16_t interval,uint16_t offset,uint8_t win,uint16_t instant)
{
    memset(p,0,12); p[1]=win; ble_put16(p+2,offset); ble_put16(p+4,interval);
    ble_put16(p+8,500); ble_put16(p+10,instant);
}
static void clean(void)
{
    ble_conn_t zero={0};
    assert(!ble_link.instant_kind && !ble_link.instant_programmed && !ble_link.instant_timed &&
        !ble_link.instant_time && !ble_link.instant && !ble_link.instant_received &&
        !ble_link.pending_serial && !memcmp(&ble_link.pending,&zero,sizeof zero));
}
static void matrix(void)
{
    static const uint16_t intervals[][2]={{12,6},{12,24},{6,12},{24,6}};
    unsigned cases=0;
    for(unsigned pair=0;pair<4;pair++) for(unsigned offset=0;offset<3;offset++)
    for(unsigned maxwin=0;maxwin<2;maxwin++) for(unsigned far=0;far<2;far++) {
        uint16_t old=intervals[pair][0],next=intervals[pair][1],instant=far ? 16400 : 6;
        uint16_t off=offset==0 ? 0 : offset==1 ? next/2u : next;
        uint8_t win=maxwin ? (next<9 ? (uint8_t)(next-1u) : 8u) : 1u,p[12];
        uint32_t anchor=begin(old,(cases&1u) ? 0xffff0000u : 24000u),ti,tw;
        packet(0,anchor,0,0); encode(p,next,off,win,instant); packet(0,anchor,p,12);
        assert(ble_link.instant_kind==1 && ble_link.instant_received==0 && !sim_applies);
        assert(sim_registered_instant==instant);
        for(unsigned event=0;event<instant;event++) {
            if(event) packet((uint16_t)event,anchor,0,0);
            close_event((uint16_t)event,anchor);
            assert(ble_link.diag.event==event && ble_link.conn.interval==old && !updates);
            assert(sim_applies==(event==instant-1u ? 1u : 0u));
            if(event+1u<instant) anchor+=(uint32_t)old*30000u;
        }
        ti=anchor+(uint32_t)old*30000u; tw=ti+(uint32_t)off*30000u;
        assert(sim_apply_kind==1 && sim_apply_event==instant-1u && sim_applied.interval==next &&
            sim_applied.offset==off && sim_applied.win==win && ble_link.instant_programmed);
        assert(ble_link.instant_time==ti && ble_link.anchor==tw && ble_link.anchor_event==instant);
        assert(ble_link.time.win_reset==tw && sim_windows>=win*1250u+625u);
        sim_ticks=ti-1u; work(); drain(); assert(!updates && ble_link.conn.interval==old);
        sim_ticks=ti; work(); drain(); assert(updates==1 && ble_link.conn.interval==next); clean();
        assert(ble_link.diag.update[0].outcome==BLE_UPDATE_APPLIED);
        assert(ble_link.time.sup==ti+500u*240000u);
        packet(instant,tw,0,0); assert(sim_windows==2u*ble_link.widen+50u);
        /* Immediately start another procedure in event I, without an idle gap. */
        encode(p,old,0,1,(uint16_t)(instant+2u)); packet(instant,tw,p,12);
        assert(ble_link.instant_kind==1 && ble_link.pending.interval==old);
        close_event(instant,tw); tw+=(uint32_t)next*30000u;
        packet((uint16_t)(instant+1u),tw,0,0); close_event((uint16_t)(instant+1u),tw);
        sim_ticks=tw+(uint32_t)next*30000u; work(); drain();
        assert(updates==2 && ble_link.conn.interval==old && !ble_link.diag.late_instants); clean();
        cases++;
    }
    printf("central update matrix: %u cases + %u immediate updates, near/far, tick wrap, WinOffset 0/mid/max, WinSize 1/max: PASS\n",cases,cases);
}
static void invalid_and_late(void)
{
    uint8_t p[12],map[8]={1,3,0,0,0,0,6,0}; uint32_t a;
    a=begin(12,24000); packet(0,a,0,0);
    encode(p,6,6,5,6); packet(0,a,p,11);
    assert(ble_link.diag.ignored_reason==BLE_IGNORE_LENGTH && !ble_link.instant_kind);
    for(unsigned bad=0;bad<6;bad++) {
        encode(p,6,6,5,6);
        if(bad==0) p[1]=6; /* legal for old interval 12, illegal for NEW 6 */
        if(bad==1) ble_put16(p+2,7);
        if(bad==2) ble_put16(p+4,5);
        if(bad==3) ble_put16(p+6,500);
        if(bad==4) ble_put16(p+8,9);
        if(bad==5) { ble_put16(p+6,6); ble_put16(p+8,10); }
        packet(0,a,p,12); assert(!ble_link.instant_kind && ble_link.state==BLE_LINK_UP);
        assert(ble_link.diag.ignored_reason==BLE_IGNORE_PARAMS);
    }
    assert(ble_link.diag.ignore_reasons[BLE_IGNORE_PARAMS]==6 && !sim_applies);
    encode(p,24,24,8,6); packet(0,a,p,12); /* > OLD offset is legal */
    assert(ble_link.instant_kind==1);
    packet(0,a,map,8); packet(0,a,p,12);
    assert(ble_link.diag.ignore_reasons[BLE_IGNORE_PENDING]==2);
    /* Skip the required close: never retain the pending record silently. */
    packet(6,a+6u*12u*30000u,0,0); close_event(6,a+6u*12u*30000u);
    assert(last_reason==0x28 && disconnects==1 && !sim_applies); clean();
    a=begin(12,24000); packet(0,a,0,0); encode(p,6,0,1,6); packet(0,a,p,12);
    /* No more event IRQs: autonomous worker must catch the 1 ms guard. */
    sim_ticks=a+6u*12u*30000u-1000u*24u+1u; work(); drain();
    assert(last_reason==0x28 && disconnects==1); clean();
    for(unsigned kind=0;kind<2;kind++) {
        a=begin(12,24000); packet(0,a,0,0); close_event(0,a);
        if(kind) { ble_put16(map+6,1); packet(0,a,map,8); }
        else { encode(p,6,0,1,1); packet(0,a,p,12); }
        work(); drain(); assert(last_reason==0x28); clean();
    }
    for(unsigned kind=0;kind<2;kind++) {
        a=begin(12,24000); packet(0,a,0,0);
        if(kind) { ble_put16(map+6,16400); packet(0,a,map,8); }
        else { encode(p,6,0,1,16400); packet(0,a,p,12); }
        assert(!ble_link.instant_timed); /* Outside the absolute tick horizon. */
        /* Lost close IRQs, delayed RX (no trustworthy anchor) still witnesses
         * that the instant was passed. No silent far-pending procedure. */
        for(unsigned event=1;event<=16401;event++) {
            sim_ticks=a+event*12u*30000u+80u*24u;
            assert(ble_ll_rx_delayed(1,1,0,sim_ticks,(uint16_t)event,1)); work();
            if(event<16401) assert(ble_link.instant_kind && !ble_link.instant_timed);
        }
        drain(); assert(last_reason==0x28 && !sim_applies); clean();
    }
    for(unsigned kind=0;kind<2;kind++) {
        a=begin(12,0xffff0000u); packet(0,a,0,0); close_event(0,a); a+=12u*30000u;
        if(kind) { ble_put16(map+6,2); packet(1,a,map,8); }
        else { encode(p,6,0,1,2); packet(1,a,p,12); }
        assert(ble_link.instant_kind==(kind ? 2 : 1));
        close_event(1,a); assert(sim_applies==1 && ble_link.instant_programmed);
        sim_ticks=a+12u*30000u; work(); drain(); clean();
        assert(!ble_link.diag.late_instants && (kind ? ble_map_count(ble_link.conn.map)==2 : updates==1));
        /* Duplicate event close cannot project the future anchor backwards. */
        { uint32_t anchor=ble_link.anchor; ble_ll_event(1,sim_ticks); assert(ble_link.anchor==anchor); }
    }
    puts("invalid NEW-interval limits ignored; busy/length diagnostics; both late kinds/missing-close watchdog and I-1 receipt: PASS");
}
static void reconnects(void)
{
    static const uint8_t reasons[]={8,0x3e,0x28,0x3d,0x13,0x16,0x1f};
    uint8_t map[8]={1,3,0,0,0,0,6,0},p[12];
    for(unsigned kind=0;kind<3;kind++) for(unsigned cause=0;cause<sizeof reasons;cause++) {
        uint32_t a=begin(12,0xffff0000u),epoch=ble_link.epoch;
        packet(0,a,0,0); close_event(0,a);
        encode(p,6,6,5,kind==2 ? 6 : 1000);
        if(kind==0) ble_put16(map+6,1000);
        packet(1,a+12u*30000u,kind==0 ? map : p,kind==0 ? 8 : 12);
        assert(ble_link.instant_kind==(kind==0 ? 2 : 1));
        if(kind==2) { /* Also close after programming, before T_inst/T_win. */
            for(unsigned e=1;e<6;e++) {
                a+=12u*30000u; packet((uint16_t)e,a,0,0); close_event((uint16_t)e,a);
            }
            assert(ble_link.instant_programmed && ble_link.time.win_pending==3);
        }
        if(reasons[cause]==8) {
            /* Inject an old deadline just before the armed reset: expiry wins. */
            if(kind==2) ble_link.time.sup=ble_link.instant_time-1u;
            sim_ticks=ble_link.time.sup; work();
        }
        else ble_ll_close(reasons[cause]);
#if FM1B_BLE_RELIABILITY
        /* A first local 0x3D completes its classifier before normal closing. */
        if(ble_reliability_failed()) work();
#endif
        clean(); assert(!ble_link.anchor && !ble_link.time.win_pending && !ble_link.acc && !ble_link.closed_valid);
        work(); drain(); assert(disconnects==1 && last_reason==reasons[cause]);
        { ble_conn_t c=sim_connection(); c.interval=12; assert(ble_ll_create(&c,sim_ticks)); }
        drain(); a=ble_link.time.first;
        assert(ble_link.epoch!=epoch && !ble_link.diag.event && !ble_link.peer_stop && !ble_link.local_terminate);
        clean(); assert(!ble_ll_pending());
        for(unsigned e=0;e<100;e++) {
            packet((uint16_t)e,a,0,0); close_event((uint16_t)e,a);
            assert(ble_link.diag.event==e && ble_link.anchor==a); a+=12u*30000u;
        }
        assert(!ble_ll_pending());
    }
    puts("map/update pending and programmed: 7 closing causes, supervision expiry, reconnect timing/epoch/queues clean: PASS");
}
static void long_session(void)
{
    uint32_t a=begin(12,0xffff0000u),first=a; uint8_t p[12];
    for(unsigned e=0;e<40000;e++) {
        packet((uint16_t)e,a,0,0);
        if(e==6000) { encode(p,12,0,1,6006); packet((uint16_t)e,a,p,12); }
        close_event((uint16_t)e,a);
        if(ble_link.diag.event!=e || ble_link.diag.events!=e+1u)
            fprintf(stderr,"phase gap at event %u: diag event %u, serviced %u\n",e,ble_link.diag.event,(unsigned)ble_link.diag.events);
        assert(ble_link.diag.event==e && ble_link.diag.events==e+1u);
        a+=12u*30000u;
    }
    assert(ble_link.time.first==first && updates==1 && !ble_link.diag.late_instants);
    puts("600-second event grid crosses stale-first half-wrap and multiple tick wraps with no frozen counters: PASS");
}
#if FELUCCA_BLE_STAGE >= 3
static int slot_opcode(uint8_t opcode)
{
    for(unsigned i=0;i<2;i++) if(sim_slot[i]>=0 && ble_ll_tx_data((unsigned)sim_slot[i])[0]==opcode) return (int)i;
    return -1;
}
static void encrypted_updates(void)
{
    static const uint16_t intervals[][2]={{12,6},{12,24},{6,12},{24,6}};
    for(unsigned pair=0;pair<4;pair++) for(unsigned off=0;off<3;off++) {
        uint8_t req[23]={3},ltk[16]={0},skd[16],key_le[16],sk[16],iv[8],rsp[5]={6},p[16];
        uint16_t old=intervals[pair][0],next=intervals[pair][1];
        uint16_t offset=off==0 ? 0 : off==1 ? next/2u : next;
        uint32_t a=begin(old,0xffff0000u); int slot;
        packet(0,a,0,0);
        for(unsigned i=1;i<23;i++) req[i]=(uint8_t)i;
        packet(0,a,req,23);
        for(unsigned i=0;i<64 && slot_opcode(4)<0;i++) { ble_entropy_step(); work(); }
        slot=slot_opcode(4); assert(slot>=0);
        memcpy(skd,req+11,8); memcpy(skd+8,sim_air[slot]+1,8);
        memcpy(iv,req+19,4); memcpy(iv+4,sim_air[slot]+9,4);
        assert(ble_aes_le(ltk,skd,key_le));
        for(unsigned i=0;i<16;i++) sk[i]=key_le[15u-i];
        sim_ack((unsigned)slot); drain();
        assert(!ble_ll_ltk_reply(BLE_HANDLE,ltk,0)); work();
        slot=slot_opcode(5); assert(slot>=0); sim_ack((unsigned)slot); work();
        assert(ble_ccm(sk,iv,0,1,3,rsp,1,0)); packet(0,a,rsp,5);
        slot=slot_opcode(6); assert(slot>=0); sim_ack((unsigned)slot); work(); drain();
        { ble_security_diag_t d; ble_security_get_diag(&d); assert(d.encrypted && d.rx_counter==1); }
        encode(p,next,offset,next<9 ? (uint8_t)(next-1u) : 8u,6);
        assert(ble_ccm(sk,iv,1,1,3,p,12,0)); packet(0,a,p,16);
        assert(ble_link.instant_kind==1);
        for(unsigned event=0;event<6;event++) {
            if(event) packet((uint16_t)event,a,0,0);
            close_event((uint16_t)event,a); a+=(uint32_t)old*30000u;
        }
        sim_ticks=a; work(); drain(); assert(updates==1 && ble_link.conn.interval==next); clean();
        a+=(uint32_t)offset*30000u; packet(6,a,0,0); close_event(6,a);
        assert(!ble_link.diag.late_instants);
        { ble_security_diag_t d; ble_security_get_diag(&d); assert(d.encrypted && d.rx_counter==2 && !d.mic_failures); }
    }
    puts("12 encrypted central interval changes, WinOffset 0/mid/max and maximum WinSize, real CCM/ordered control path: PASS");
}
#endif
int main(int argc,char **argv)
{
    (void)argv;
    if(argc>1) { long_session(); return 0; }
    setvbuf(stdout,0,_IONBF,0);
    matrix(); invalid_and_late(); reconnects(); long_session();
#if FELUCCA_BLE_STAGE >= 3
    encrypted_updates();
#endif
    return 0;
}
