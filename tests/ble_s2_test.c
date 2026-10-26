/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ble_s2_sim.h"
#include "../open_ble/ble_midi.h"
#include "../open_ble/ble_breadcrumb.h"
static uint8_t events[1024][80],types[1024];
static uint16_t lengths[1024];
static unsigned count;
static const uint8_t public_addr[6]={0x58,0x65,0x67,0x8b,0x40,0x62};
static void frame_window_and_breadcrumb(void)
{
    _Alignas(4) uint16_t frame[BLE_FRAME_SIZE/2];
    const uint32_t windows[]={0,50,1250,2500,65535,65536,0x12345678u,0xffffffffu};
    volatile ble_breadcrumb_t b;
    unsigned i,j;
    /* Exercise the actual HAL store helper under UBSan alignment checks.
     * Its target address is 2 modulo 4, including in the bench DMA layout. */
    assert(((uintptr_t)(frame+BF_WIN/2)&3u)==2u);
    for(i=0;i<sizeof windows/sizeof windows[0];i++) {
        for(j=0;j<BLE_FRAME_SIZE/2;j++) frame[j]=0xa55a;
        ble_frame_window_set(frame,windows[i]);
        assert(frame[BF_WIN/2]==(uint16_t)windows[i]);
        assert(frame[BF_WIN/2+1]==(uint16_t)(windows[i]>>16));
        for(j=0;j<BLE_FRAME_SIZE/2;j++)
            if(j!=BF_WIN/2 && j!=BF_WIN/2+1) assert(frame[j]==0xa55a);
    }
    memset((void *)&b,0xa5,sizeof b); ble_breadcrumb_init(&b);
    assert(b.magic==BLE_BREADCRUMB_MAGIC && !b.fault && !b.fault_count);
    ble_breadcrumb_mark(&b,BLE_STAGE_WINDOW,4,2500,0xfffffff0u);
    ble_breadcrumb_fault(&b,3);
    assert(b.fault==0x130409c4u && b.fault_ticks==0xfffffff0u && b.fault_count==3);
    /* Warm reset and new traffic retain the fault and its crash association. */
    ble_breadcrumb_init(&b); ble_breadcrumb_mark(&b,BLE_STAGE_HOST,1,0,0x100u);
    assert(b.last==0x40010000u && b.fault==0x130409c4u && b.fault_count==3);
    b.magic=0; ble_breadcrumb_init(&b);
    assert(!b.last && !b.fault && !b.fault_count);
}
static void host(uint8_t type,const uint8_t *p,uint16_t n)
{
    assert(count<1024 && n<=80 && (type==2 || type==4));
    memcpy(events[count],p,n); types[count]=type; lengths[count++]=n;
}
static uint8_t command(uint16_t opcode,const uint8_t *p,unsigned n)
{
    uint8_t b[35]={0}; unsigned before=count;
    ble_put16(b,opcode); b[2]=(uint8_t)n; if(n) memcpy(b+3,p,n);
    assert(!ble_hci_send(1,b,(uint16_t)(n+3u)));
    for(unsigned i=0;i<64 && count==before;i++) ble_worker();
    assert(count>before);
    return events[count-1][events[count-1][0]==15 ? 2 : 5];
}
static void ready(void)
{
    sim_reset(); count=0; ble_init(public_addr,host);
    assert(!command(0x0c01,(uint8_t[8]){255,255,255,255,255,255,255,255},8));
    assert(!command(0x2001,(uint8_t[8]){255,255,255,255,255,255,255,255},8)); count=0;
}
static void logic(void)
{
    ble_conn_t c=sim_connection(),out; uint8_t request[34]={0};
    uint8_t map[5]={255,255,15,0,0},unmapped=0; unsigned i,k;
    assert(ble_map_count(map)==20);
    for(k=5;k<=16;k+=11) {
        unmapped=0;
        for(i=0;i<200;i++) {
            unsigned u=((i+1)*k)%37,expected=u<20 ? u : u%20;
            assert(ble_csa1(&unmapped,(uint8_t)k,map)==expected && unmapped==u);
        }
    }
    memset(map,255,5); assert(ble_map_count(map)==37);
    assert(ble_conn_params_valid(&c)); c.timeout=10; assert(ble_conn_params_valid(&c));
    c.latency=1; assert(!ble_conn_params_valid(&c)); c=sim_connection();
    c.interval=6; c.win=6; assert(!ble_conn_params_valid(&c)); c.win=5; assert(ble_conn_params_valid(&c));
    c=sim_connection(); assert(ble_widen(&c)==62); c.interval=12; assert(ble_widen(&c)==32);
    c.interval=6; assert(ble_widen(&c)==16); c.interval=12; c.sca=1; assert(ble_widen(&c)==24);
    c.interval=3435; c.latency=0; c.sca=0; assert(ble_widen(&c)==8588); c=sim_connection();
    ble_put32(request+12,c.aa); request[16]=0x56; request[17]=0x34; request[18]=0x12;
    request[19]=1; ble_put16(request+22,24); ble_put16(request+26,500);
    memset(request+28,255,4); request[32]=0xff; request[33]=5;
    assert(ble_connect_decode(&out,request,34,0) && out.map[4]==31);
    for(i=0;i<34;i++) assert(!ble_connect_decode(&out,request,(uint8_t)i,0));
    ble_put32(request+12,0x8e89bed6); assert(!ble_connect_decode(&out,request,34,0));
    assert(ble_instant_valid(3,65534)); assert(ble_instant_valid(4,2)); assert(!ble_instant_valid(4,3));
    assert(!ble_instant_valid(4,4)); assert(ble_instant_valid(0,65534));
    assert(ble_rx_class(1,27)==0 && ble_rx_class(0x7101,27)==0);
    assert(ble_rx_class(5,255)==1 && ble_rx_class(13,255)==1 && ble_rx_class(9,27)==2);
    assert(ble_rx_class(1,28)==3 && ble_rx_class(0,0)==3 && ble_rx_class(3,0)==3);
    { int next,f=423; unsigned drift; unsigned expected[]={0,0,0,1};
      for(i=0;i<4;i++) { assert(ble_missed_step(f,20,525,&next,&drift)); assert(next==f-100 && drift==expected[i]); f=next; }
      assert(!ble_missed_step(123,1,525,&next,&drift));
      assert(ble_missed_step(-600,20,525,&next,&drift) && next==-700 && !drift);
      assert(!ble_missed_step(-2048,20,10000,&next,&drift)); }
}
static void timelines(void)
{
    ble_timeline_t t; ble_conn_t c=sim_connection(); unsigned cls; uint32_t origin;
    for(origin=0; ;origin=0xffff0000u) {
        c=sim_connection();
        ble_timeline_create(&t,&c,origin);
        assert(t.sup==origin+5000u*24000u && t.first==origin+1250u*24u);
        assert(t.est_end==origin+153500u*24u && t.cutoff==origin+181250u*24u);
        assert(!ble_timeline_check(&t,&c,t.est_end-1)); assert(ble_timeline_check(&t,&c,t.est_end)==0x3e);
        for(cls=0;cls<3;cls++) {
            ble_timeline_create(&t,&c,origin);
            assert(!ble_timeline_rx(&t,&c,(uint8_t)cls,origin+152000u*24u) && t.established);
            assert(t.sup==origin+(cls==1 ? 5000000u : 5152000u)*24u);
            assert(!ble_timeline_check(&t,&c,t.sup-1)); assert(ble_timeline_check(&t,&c,t.sup)==8);
            ble_timeline_create(&t,&c,origin);
            assert(ble_timeline_rx(&t,&c,(uint8_t)cls,t.cutoff)==0x3e && !t.established);
        }
        ble_timeline_create(&t,&c,origin); assert(!ble_timeline_rx(&t,&c,1,origin+1900u*24u));
        assert(t.sup==origin+5000000u*24u);
        c.offset=3; c.win=2; ble_timeline_create(&t,&c,origin);
        assert(t.first==origin+5000u*24u && t.est_end==origin+158500u*24u && t.cutoff==origin+185000u*24u);
        c=sim_connection(); c.timeout=100; t.established=1; t.sup=1240000u*24u; ble_timeline_arm(&t,240000u*24u,243750u*24u,100);
        assert(!ble_timeline_check(&t,&c,243750u*24u)); assert(t.sup==1243750u*24u);
        if(origin) break;
    }
}
static void r4_ordering(void)
{
    ble_conn_t c=sim_connection(); ble_timeline_t t; unsigned i; uint8_t value=1,type,p[32]; uint16_t n;
    for(i=0;i<2;i++) {
        uint32_t base=i ? 0xffff0000u : 0;
        ble_timeline_create(&t,&c,base); t.established=1; t.sup=base+160000u*24u;
        ble_timeline_arm(&t,base+180000u*24u,base+185000u*24u,10);
        assert(ble_timeline_check(&t,&c,base+185000u*24u)==8); /* never rescued */
        ble_timeline_arm(&t,base+160000u*24u,base+165000u*24u,10);
        assert(ble_timeline_check(&t,&c,base+185000u*24u)==8); /* expiry wins tie */
        ble_timeline_arm(&t,base+150000u*24u,base+155000u*24u,10);
        assert(!ble_timeline_check(&t,&c,base+156000u*24u));
        assert(t.sup==base+255000u*24u);
        assert(!ble_timeline_rx_delayed(&t,0,base+80000u*24u,base+170000u*24u,1));
        assert(t.sup==base+255000u*24u); /* lower-bound refresh never regresses */
    }
    ready(); ble_ll_create(&c,sim_ticks); ble_worker();
    for(i=0;i<19;i++) assert(ble_ll_rx(1,0x102,&value,sim_ticks,0));
    assert(ble_ll_rx_marker(1) && ble_ll_rx_marker(2));
    assert(!ble_ll_rx_marker(1)); /* two reserved marker entries */
    ble_ll_worker(); ble_ll_worker(); assert(ble_link.diag.epoch_markers==2);
    for(i=0;i<19;i++) assert(ble_ll_poll(&type,p,&n) && type==2 && p[4]==1);
    ble_ll_close(8); ble_ll_worker(); ble_worker();
    ble_ll_create(&c,sim_ticks); { ble_link_diag_t d; ble_ll_get_diag(&d); assert(d.reason==8); }
    ready();
    { uint8_t mask[11]={1,12,8},reset[3]={3,12,0}; unsigned before=count;
      assert(!ble_hci_send(1,mask,sizeof mask)); assert(!ble_hci_send(1,reset,3));
      ble_worker(); assert(count==before+1 && events[count-1][5]==0x0c);
      for(unsigned i=0;i<64 && count==before+1;i++) ble_worker();
      assert(count==before+2 && ble_get16(events[count-1]+3)==0x0c03 && !events[count-1][5]); }
}
static void controls_and_credits(void)
{
    ble_conn_t c=sim_connection(); uint8_t acl[31]={0x50,0x20,27,0},type,p[32]; uint16_t n; unsigned i,id;
    ready(); ble_ll_create(&c,sim_ticks); ble_worker(); assert(events[0][2]==1 && lengths[0]==21);
    for(i=0;i<5;i++) { acl[4]=(uint8_t)i; assert(!ble_hci_send(2,acl,sizeof acl)); }
    assert(ble_hci_send(2,acl,sizeof acl)); assert(sim_slot[0]>=0 && sim_slot[1]>=0);
    assert(ble_ll_rx(9,1,0,sim_ticks+50000u,0)); assert(!ble_link.completed);
    id=(unsigned)sim_slot[0]; sim_ack(0); assert(ble_link.completed==1); ble_ll_tx_ack(id); assert(ble_link.completed==1);
    assert(ble_ll_poll(&type,p,&n) && p[0]==0x13 && ble_get16(p+5)==1);
    assert(!ble_hci_send(2,acl,sizeof acl));
    ble_ll_close(0x13); assert(ble_hci_send(2,acl,sizeof acl)); sim_stop_result=0; ble_ll_worker();
    assert(ble_link.state==BLE_LINK_CLOSING && !ble_ll_poll(&type,p,&n));
    sim_ack(1); sim_stop_result=1; ble_ll_worker();
    assert(ble_ll_poll(&type,p,&n) && p[0]==0x13);
    assert(ble_ll_poll(&type,p,&n) && p[0]==5 && p[5]==0x13);
    assert(!ble_ll_poll(&type,p,&n));
    sim_slot[0]=sim_slot[1]=-1; ble_ll_create(&c,sim_ticks); assert(!ble_link.completed);
    assert(ble_ll_poll(&type,p,&n) && p[0]==0x3e);
    assert(ble_ll_features(BLE_HANDLE)==0 && ble_ll_features(BLE_HANDLE)==0x0c);
    sim_advance_us(800000); /* Establishment deadline must win over a pending feature timer. */
    assert(ble_link.reason==0x3e);
    ready(); ble_ll_create(&c,sim_ticks); ble_worker();
    assert(ble_ll_rx(1,1,0,sim_ticks+50000,0));
    assert(ble_ll_features(BLE_HANDLE)==0); sim_advance_us(800000);
    assert(ble_ll_poll(&type,p,&n) && p[2]==4 && p[3]==0x1a);
    { uint8_t f[9]={8,255,1,2,3,4,5,6,7};
      assert(ble_ll_rx(1,0x903,f,sim_ticks,1)); ble_ll_worker(); assert(ble_link.features_known);
      assert(!ble_ll_features(BLE_HANDLE)); ble_ll_worker();
      assert(ble_ll_poll(&type,p,&n) && p[3]==0 && p[6]==0 && p[7]==1 && p[13]==7); }
}
static void instants_and_pressure(void)
{
    ble_conn_t c=sim_connection(); unsigned i; uint8_t type,p[32],value; uint16_t n;
    uint8_t update[12]={0,2,3,0,12,0,0,0,100,0,8,0};
    ready(); sim_ticks=0; ble_ll_create(&c,0); ble_link.time.established=1;
    ble_link.time.first=0; ble_link.anchor=0; ble_link.anchor_seen=1; ble_worker();
    assert(ble_ll_rx(1,0xc03,update,0,0)); ble_ll_worker(); assert(ble_link.instant_kind==1);
    for(i=0;i<8;i++) { sim_ticks=(i*30000u+1000u)*24u; ble_ll_event((uint16_t)i,sim_ticks); }
    assert(sim_applies==1 && ble_link.conn.interval==24 && ble_link.time.sup==5000000u*24u);
    sim_ticks=243750u*24u; ble_ll_worker();
    assert(ble_link.conn.interval==12 && ble_link.time.sup==1243750u*24u);
    assert(ble_ll_poll(&type,p,&n) && p[2]==3 && ble_get16(p+6)==12);
    sim_ticks=1243750u*24u-1u; ble_ll_worker(); assert(ble_link.state==BLE_LINK_UP || ble_link.state==BLE_LINK_CREATED);
    sim_ticks++; ble_ll_worker(); assert(ble_link.reason==8);
    /* Apply a channel map across event-counter wrap, then reject a late one. */
    ready(); ble_ll_create(&c,sim_ticks); ble_link.time.established=1;
    ble_link.time.first=sim_ticks; ble_link.anchor=sim_ticks; ble_link.anchor_event=65532;
    ble_link.anchor_seen=1; ble_link.closed_n=65531; ble_link.closed_valid=1;
    { uint8_t map[8]={1,255,255,15,0,0,4,0};
      assert(ble_ll_rx(1,0x803,map,sim_ticks,65532)); ble_ll_worker();
      { uint32_t baseline=sim_ticks;
        for(i=0;i<9;i++) { sim_ticks=baseline+i*720000u+24000u; ble_ll_event((uint16_t)(65532u+i),sim_ticks); } }
      assert(sim_applies==1 && ble_map_count(ble_link.conn.map)==20 && ble_link.diag.map_updates==1);
      ble_put16(map+6,(uint16_t)(ble_link.closed_n+1u));
      assert(ble_ll_rx(1,0x803,map,sim_ticks,ble_link.closed_n)); ble_ll_worker(); assert(ble_link.reason==0x28); }
    /* Both software tiers full: rejection leaves the caller's slot owned by
     * the caller, and releasing one entry permits exactly one retry. */
    ready(); ble_ll_create(&c,sim_ticks); ble_worker();
    for(i=0;i<19;i++) { value=(uint8_t)i; assert(ble_ll_rx(1,0x102,&value,sim_ticks,0)); }
    ble_ll_worker();
    for(i=19;i<38;i++) { value=(uint8_t)i; assert(ble_ll_rx(1,0x102,&value,sim_ticks,0)); }
    value=38; assert(!ble_ll_rx(1,0x102,&value,sim_ticks,0));
    assert(ble_ll_poll(&type,p,&n) && type==2 && p[4]==0); ble_ll_worker();
    assert(ble_ll_rx(1,0x102,&value,sim_ticks,0));
    for(i=1;i<39;i++) {
        ble_ll_worker(); assert(ble_ll_poll(&type,p,&n) && type==2 && p[4]==i);
    }
    assert(!ble_ll_poll(&type,p,&n) && ble_link.diag.rx_backpressure==1);
    ready(); ble_ll_create(&c,sim_ticks); ble_worker();
    { uint8_t feature[9]={8,1,0,0,0,0,0,0,0};
      assert(ble_ll_rx(1,0x903,feature,sim_ticks,0)); ble_ll_worker();
      assert(!ble_ll_disconnect(BLE_HANDLE,0x13)); assert(sim_slot[0]>=0 && sim_slot[1]>=0);
      sim_ack(0); assert(ble_link.state<BLE_LINK_CLOSING); sim_ack(1); assert(ble_link.state==BLE_LINK_CLOSING && ble_link.reason==0x16); }
    ready(); c.timeout=10; ble_ll_create(&c,sim_ticks); ble_worker();
    assert(!ble_ll_disconnect(BLE_HANDLE,0x13));
    sim_ticks+=50000u*24u; assert(ble_ll_rx(9,1,0,sim_ticks,0));
    sim_advance_us(50000); assert(ble_link.reason==8 && ble_link.diag.terminate_timeouts==1);
}
static void hci(void)
{
    uint8_t data[32]={0},cmd[3]={1,16,0}; unsigned before,i;
    static const uint16_t mandatory[]={0x0c03,0x1001,0x1002,0x1003,0x1009,0x0c01,0x2001,0x2002,0x2003,0x2008,0x2009,0x2006,0x200a,0x2016,0x0406};
    ready();
    assert(!command(0x1002,0,0)); assert(events[count-1][6]==0x20 && events[count-1][6+27]==0x20);
    for(i=0;i<sizeof mandatory/sizeof mandatory[0];i++) {
        if(mandatory[i]==0x0c03) continue;
        command(mandatory[i],0,0); assert(events[count-1][5]!=1 || events[count-1][0]==15);
    }
    assert(command(0x2018,0,0)==1 && lengths[count-1]==6);
    assert(command(0x2016,data,2)==2 && events[count-1][0]==15);
    assert(command(0x0406,data,3)==2 && events[count-1][0]==15);
    assert(command(0xffff,0,0)==1);
    assert(ble_hci_send(1,cmd,2)); before=count;
    for(i=0;i<100;i++) assert(!ble_hci_send(1,cmd,3));
    for(i=0;i<100;i++) ble_worker();
    assert(count-before==100);
    assert(!command(0x0c03,0,0));
}
static void masks_and_controls(void)
{
    ble_conn_t c=sim_connection(); unsigned before,i; uint8_t p[8]={0},acl[5]={0x50,0x20,1,0,1};
    ready(); assert(!command(0x0c01,p,8) && !command(0x2001,p,8)); before=count;
    ble_ll_create(&c,sim_ticks); ble_worker(); assert(count==before); /* masked creation */
    assert(!ble_hci_send(2,acl,sizeof acl)); sim_ack(0); ble_worker();
    assert(count==before+1 && events[count-1][0]==0x13); /* unmaskable NCP */
    assert(!command(0x0406,(uint8_t[]){0x50,0,0x13},3)); before=count;
    sim_ack(0); ble_ll_worker(); ble_worker(); assert(count==before && ble_link.state==BLE_LINK_IDLE);
    assert(!command(0x0c03,0,0)); before=count;
    ble_ll_create(&c,sim_ticks); ble_worker(); assert(count==before); /* Reset restored defaults */
    ready(); ble_ll_create(&c,sim_ticks); ble_worker();
    { uint8_t version[6]={12,6,1,0,2,0};
      assert(ble_ll_rx(1,0x603,version,sim_ticks,0)); ble_ll_worker();
      assert(sim_slot[0]>=0 && ble_ll_tx_data((unsigned)sim_slot[0])[0]==12);
      version[1]=9; assert(ble_ll_rx(1,0x603,version,sim_ticks,0)); ble_ll_worker();
      assert(sim_slot[1]<0 && ble_link.peer_version[0]==6); sim_ack(0); }
    { uint8_t unknown[9]={0x15,0,0,0,0,0,0,0,0};
      assert(ble_ll_rx(1,0x903,unknown,sim_ticks,0)); ble_ll_worker();
      assert(sim_slot[0]>=0 && ble_ll_tx_len((unsigned)sim_slot[0])==2 && ble_ll_tx_data((unsigned)sim_slot[0])[0]==7 && ble_ll_tx_data((unsigned)sim_slot[0])[1]==0x15);
      sim_ack(0);
      for(i=4;i<=6;i++) { unknown[0]=(uint8_t)i; assert(ble_ll_rx(1,0x103,unknown,sim_ticks,0)); ble_ll_worker(); assert(sim_slot[0]<0 && sim_slot[1]<0); }
    }
    for(i=0;i<4;i++) {
        uint8_t bad[5]={0x50,0x20,1,0,1};
        if(i==0) bad[1]=0x30;
        if(i==1) bad[1]=0x60;
        if(i==2) bad[0]=0x51;
        if(i==3) bad[2]=0;
        assert(ble_hci_send(2,bad,sizeof bad));
    }
    assert(!ble_link.completed);
    ready(); c.latency=4; ble_ll_create(&c,sim_ticks); ble_worker();
    assert(!ble_hci_send(2,acl,sizeof acl));
    for(i=0;i<8;i++) {
        sim_ticks+=720000u;
        assert(ble_ll_rx(9,1,0,sim_ticks,(uint16_t)i));
        ble_ll_event((uint16_t)i,sim_ticks); assert(!sim_latency_enabled);
    }
    sim_ack(0); sim_ticks+=720000u;
    assert(ble_ll_rx(1,1,0,sim_ticks,8)); ble_ll_event(8,sim_ticks); assert(sim_latency_enabled);
}
static void reset_live(void)
{
    ble_conn_t c=sim_connection(); uint8_t acl[5]={0x50,0x20,1,0,42};
    uint8_t reset[3]={3,12,0}; unsigned i,before; uint32_t epoch;
    ready(); ble_ll_create(&c,sim_ticks); ble_worker(); epoch=ble_link.epoch;
    for(i=0;i<5;i++) assert(!ble_hci_send(2,acl,sizeof acl));
    assert(!ble_ll_features(BLE_HANDLE)); before=count; sim_stop_result=0;
    assert(!ble_hci_send(1,reset,sizeof reset)); ble_worker();
    assert(count==before && ble_link.state==BLE_LINK_CLOSING);
    sim_ack(0); ble_ll_worker(); ble_worker(); assert(count==before);
    sim_stop_result=1;
    for(i=0;i<64 && count==before;i++) ble_worker();
    assert(count==before+1 && events[count-1][0]==14 && ble_get16(events[count-1]+3)==0x0c03 && !events[count-1][5]);
    assert(ble_link.state==BLE_LINK_IDLE && !ble_link.completed && !ble_link.feature_pending && ble_link.epoch!=epoch);
    sim_slot[0]=sim_slot[1]=-1; ble_ll_create(&c,sim_ticks); ble_worker();
    assert(count==before+1); /* default masks and no old NCP/features/DC */
    for(i=0;i<5;i++) assert(!ble_hci_send(2,acl,sizeof acl));
    assert(ble_hci_send(2,acl,sizeof acl));
}
static uint8_t bytes[1024]; static uint16_t timestamps[1024]; static unsigned byte_n;
static void byte(uint8_t b,uint16_t ts,void *arg) { (void)arg; assert(byte_n<1024); timestamps[byte_n]=ts; bytes[byte_n++]=b; }
static void midi(void)
{
    ble_midi_decoder_t d={0}; uint8_t out[8]; int n; unsigned i;
    uint8_t running[]={0x80,0xfe,0x90,60,100,0xff,61,101,0x80,62,102};
    byte_n=0; assert(ble_midi_decode(&d,running,sizeof running,byte,0));
    assert(byte_n==9 && bytes[0]==0x90 && bytes[3]==0x90 && bytes[6]==0x90);
    assert(timestamps[0]==126 && timestamps[3]==127 && timestamps[6]==128);
    { uint8_t wrap[]={0xbf,0xff,0x90,60,1,0x80,61,2}; byte_n=0;
      assert(ble_midi_decode(&d,wrap,sizeof wrap,byte,0) && timestamps[0]==8191 && timestamps[3]==0); }
    { uint8_t sx1[]={0x80,0x81,0xf0,1,2},sx2[]={0x80,3,4,0x82,0xf8,5,0x83,0xf7};
      byte_n=0; assert(ble_midi_decode(&d,sx1,sizeof sx1,byte,0) && d.sysex);
      assert(ble_midi_decode(&d,sx2,sizeof sx2,byte,0) && !d.sysex);
      assert(byte_n==8 && !memcmp(bytes,(uint8_t[]){0xf0,1,2,3,4,0xf8,5,0xf7},8)); }
    { uint8_t bad[]={0x80,0x80,0x90,60}; byte_n=0;
      assert(!ble_midi_decode(&d,bad,sizeof bad,byte,0) && !byte_n); }
    for(i=0;i<16384;i+=127) {
        byte_n=0; n=ble_midi_encode_usb(0x643c9009,i,out,sizeof out);
        assert(n==5 && ble_midi_decode(&d,out,(unsigned)n,byte,0));
        assert(byte_n==3 && bytes[0]==0x90 && timestamps[0]==(i&8191u));
    }
    { uint32_t sx[]={0x0201f004,0x00000304,0x0000f705};
      byte_n=0;
      for(i=0;i<3;i++) { n=ble_midi_encode_usb(sx[i],1,out,sizeof out); assert(n>0 && ble_midi_decode(&d,out,(unsigned)n,byte,0)); }
      assert(!d.sysex && bytes[0]==0xf0 && bytes[byte_n-1]==0xf7); }
}
int main(void)
{
    frame_window_and_breadcrumb();
    logic(); timelines(); r4_ordering(); controls_and_credits(); instants_and_pressure(); hci(); masks_and_controls(); reset_live(); midi();
    puts("BLE S2 frame alignment, breadcrumbs, logic, HCI, lifecycle, credits, and MIDI tests: PASS"); return 0;
}
