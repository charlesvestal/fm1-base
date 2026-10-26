/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* Off-target tests of the actual controller/packet/record code. No device I/O. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../open_ble/ble_s1.h"
#include "../open_ble/ble_link.h"
uint32_t ble_port_lock(void) { return 0; }
void ble_port_unlock(uint32_t k) { (void)k; }
void ble_port_refill(void) { }
void ble_port_refill_one(void) { }
void ble_port_rx_recover(void) { }
void ble_port_window(uint32_t us) { (void)us; }
void ble_port_priority(unsigned p) { (void)p; }
void ble_port_latency(uint16_t l,int en) { (void)l; (void)en; }
void ble_port_instant(uint8_t k,uint16_t i,const ble_conn_t *c) { (void)k; (void)i; (void)c; }
void ble_port_apply(uint8_t k,const ble_conn_t *c,uint16_t w,uint32_t win) { (void)k; (void)c; (void)w; (void)win; }

static uint32_t tick,starts,updates,stops,responses;
static int failure,stop_result=1,start_steps,start_failure;
static ble_adv_t air;
static uint8_t event[80];
static uint16_t event_n;
static uint8_t flash[0x100000];
static const uint8_t pub[6]={0x58,0x65,0x67,0x8b,0x40,0x62};
uint32_t ble_port_ticks(void) { return tick; }
uint32_t ble_port_ms(void) { return tick/24000; }
void ble_port_pend(void) { }
int ble_port_failed(void) { return failure; }
int ble_port_start(const ble_adv_t *a)
{
    if(start_failure) return -1;
    if(start_steps) { start_steps--; return 1; }
    air=*a; starts++; return failure ? -1 : 0;
}
void ble_port_update(const ble_adv_t *a) { air=*a; updates++; }
void ble_port_stop_begin(void) { stops++; }
int ble_port_stop_poll(void) { return stop_result; }
void ble_port_reset_stats(void) { }
static void host(uint8_t type,const uint8_t *p,uint16_t n)
{
    assert(type==4 && n<=sizeof event && p[0]==14 && p[1]==n-2 && p[2]==1);
    memcpy(event,p,n); event_n=n; responses++;
}
static int read_flash(uint32_t off,uint8_t *p,uint32_t n)
{
    assert(off>=0x93000 && off+n<=sizeof flash);
    assert(off+n<=0x94000 || off>=0xe9000); /* never scan Felucca's store */
    memcpy(p,flash+off,n); return 0;
}
static uint32_t record(uint32_t off,uint16_t id,const uint8_t *data,uint16_t n)
{
    flash[off]=(uint8_t)ble_crc16(0,data,n); flash[off+1]=(uint8_t)id;
    ble_put16(flash+off+2,(uint16_t)((n<<4)|(id>>8))); memcpy(flash+off+4,data,n);
    return off+4+n;
}
static void hex(const char *s,uint8_t *out)
{
    while (*s) { unsigned n; assert(sscanf(s,"%2x",&n)==1); *out++=(uint8_t)n; s+=2; }
}
static void logic_tests(void)
{
    static const char *sample="ff03000000000000f40300000700000011000000fcffffff3431fcfffa000000b6ffffffcefffffffffb7f37fffb7f372a00ac003600ac0040006a004a003200b7630000";
    uint8_t trim[68],zero[64]={0},addr[6],mac[6]={0x58,0xd4,0x30,0x8b,0x40,0x62};
    ble_cal_t cal;
    ble_adv_t a;
    uint8_t f[324],pdu[39],params[15]={0};
    uint32_t off;
    unsigned i;
    assert(ble_crc16(0,(const uint8_t *)"123456789",9)==0x31c3);
    assert(ble_crc16(0,zero,64)==0);
    assert(!ble_trim_valid((uint8_t[68]){0}));
    hex(sample,trim); assert(ble_trim_valid(trim));
    assert(ble_crc16(0,trim,64)==0x63b7 && (uint8_t)ble_crc16(0,trim,68)==0xa6);
    ble_derive_public(mac,addr); assert(!memcmp(addr,pub,6));
    assert(ble_public_valid(pub)); memset(addr,0,6); assert(!ble_public_valid(addr));
    memset(addr,255,6); assert(!ble_public_valid(addr) && !ble_random_valid(addr));
    memset(addr,0,6); addr[5]=192; assert(!ble_random_valid(addr)); addr[0]=1; assert(ble_random_valid(addr));
    memset(flash,255,sizeof flash); ble_put32(flash+0x93000,0x55aaaa55);
    off=record(0x93004,187,trim,68); off=record(off,106,(uint8_t[]){11,11},2);
    off=record(off,107,(uint8_t[]){1,7,4,7,11,1,7},7);
    off=record(off,108,zero,20); (void)off;
    record(0xe9000,102,mac,6); ble_read_cal(read_flash,&cal);
    assert(cal.present==31 && cal.vm_status==1 && cal.btif_status==1);
    assert(ble_trim_valid(cal.trim) && !memcmp(cal.public_addr,pub,6));
    /* Last valid outer record wins; a bad later CRC does not erase it. */
    off=record(off,106,(uint8_t[]){2,3},2);
    record(off,106,(uint8_t[]){4,5},2); flash[off]^=1;
    ble_read_cal(read_flash,&cal); assert(cal.xtal[0]==2 && cal.xtal[1]==3);
    flash[0x93000]=0; ble_read_cal(read_flash,&cal);
    assert(cal.vm_status==2 && cal.xtal[0]==11 && !(cal.present&8));
    memset(flash,255,sizeof flash); ble_put32(flash+0x93000,0x55aaaa55);
    record(0x93004,187,trim,68); flash[0x93008+64]=0; /* invalid inner + outer */
    ble_read_cal(read_flash,&cal); assert(!(cal.present&8));
    memset(flash+0x93004,0,4); ble_read_cal(read_flash,&cal); assert(cal.vm_status==3);
    for (i=0;i<37;i++) assert(2402u+ble_frequency((uint8_t)i)==(i<11 ? 2404+2*i : 2406+2*i));
    assert(ble_frequency(37)==0 && ble_frequency(38)==24 && ble_frequency(39)==78);
    ble_put16(params,160); ble_put16(params+2,320); params[13]=7;
    memset(&a,0,sizeof a); assert(ble_adv_validate(params,&a.params)==0);
    assert(a.params.max==320);
    for (i=0;i<5;i++) { params[4]=(uint8_t)i; assert(ble_adv_validate(params,&a.params)==((i==0 || i==3) ? 0 : 0x11)); }
    params[4]=0; params[13]=0; assert(ble_adv_validate(params,&a.params)==0x12);
    params[13]=128; assert(ble_adv_validate(params,&a.params)==0x12);
    params[13]=7; params[14]=1; assert(ble_adv_validate(params,&a.params)==0x11);
    params[14]=4; assert(ble_adv_validate(params,&a.params)==0x12); params[14]=0;
    for (i=1;i<8;i++) {
        unsigned j,n=0,first=0,second=0;
        uint16_t word=ble_adv_channel_word((uint8_t)i);
        for (j=0;j<3;j++) if (i&(1u<<j)) { if (!n) first=j+37; else if (n==1) second=j+37; n++; }
        assert((word&63)==first && ((word>>8)&31)==(n>1 ? second-first : 0));
    }
    params[13]=7; assert(!ble_adv_validate(params,&a.params)); memcpy(a.addr,pub,6); a.power=6;
    ble_frame_init(f,&a);
    assert(ble_get16(f+BF_ADV_CFG)==(0xc000|1250)); assert(ble_get16(f+BF_OPTIONS)==0x228);
    assert(ble_get32(f+BF_POWER)==22 && ble_get16(f+BF_WIN)==50);
    assert(!memcmp(f+BF_LOCAL,pub,6) && ble_get16(f+BF_MODE)==8);
    assert(ble_get16(f+BF_CHMAP+4)==((37<<5)|31));
    for (i=0;i<40;i++) assert(f[BF_FREQ_TBL+i]==ble_frequency((uint8_t)i));
    a.params.type=3; ble_frame_init(f,&a);
    assert(ble_get16(f+BF_ADV_CFG)==(0xc000|625) && (ble_get16(f+BF_MODE)&256));
    memset(pdu,0xcc,sizeof pdu); assert(ble_adv_payload(pdu,pub,zero,31)==37);
    assert(!memcmp(pdu,pub,6) && pdu[37]==0xcc && !ble_adv_payload(pdu,pub,zero,32));
    assert(ble_adv_header(4,1)==20);
}
static uint8_t send(uint16_t op,const uint8_t *p,uint8_t n)
{
    uint8_t packet[254]; uint32_t before=responses;
    ble_put16(packet,op); packet[2]=n; if (n) memcpy(packet+3,p,n);
    assert(!ble_hci_send(1,packet,(uint16_t)(3+n))); memset(packet,0xaa,sizeof packet);
    for(unsigned i=0;i<64 && responses==before;i++) ble_worker();
    assert(responses==before+1);
    assert(ble_get16(event+3)==op); return event[5];
}
static void controller_tests(void)
{
    uint8_t p[32]={0},raw[255]={0},map[64];
    ble_diag_t d;
    uint32_t before,i;
    ble_init(pub,host); assert(!send(0x1001,0,0) && event_n==14 && event[6]>=6);
    assert(!send(0x1003,0,0) && event_n==14 && event[10]==0x60);
    assert(!send(0x2003,0,0) && !event[6]);
    assert(!send(0x1009,0,0) && !memcmp(event+6,pub,6));
    assert(!send(0x2002,0,0) && event_n==9 && event[6]==27 && event[8]==5);
    assert(!send(0x1002,0,0)); memcpy(map,event+6,64);
    assert(map[5]==192 && map[14]==40 && map[15]==2 && map[25]==(FELUCCA_BLE_TX_DBM==127 ? 0xb7 : 0xf7) && map[26]==3 && map[0]==0x20 && map[27]==0x20 && map[28]==0x08);
    for (i=29;i<64;i++) assert(map[i]==0);
#if FELUCCA_BLE_TX_DBM == 127
    assert(send(0x2007,0,0)==1);
#else
    assert(!send(0x2007,0,0) && event[6]==(uint8_t)FELUCCA_BLE_TX_DBM);
    ble_set_power(2); assert(send(0x2007,0,0)==3); ble_set_power(6);
#endif
    assert(send(0x0c63,0,0)==1 && send(0xffff,0,0)==1);
    assert(!send(0x0c01,p,8) && !send(0x2001,p,8)); /* completions unmasked */
    p[0]=32; assert(send(0x2008,p,32)==0x12); p[0]=31;
    assert(!send(0x2008,p,32) && !send(0x2009,p,32));
    ble_put16(p,160); ble_put16(p+2,160); memset(p+4,0,11); p[13]=7;
    assert(!send(0x2006,p,15)); p[0]=1; assert(!send(0x200a,p,1) && starts==1);
    assert(air.data_len==31 && !memcmp(air.addr,pub,6));
    assert(!send(0x200a,p,1) && starts==1); assert(send(0x2006,p,15)==0x0c);
    p[0]=5; assert(!send(0x2008,p,32) && updates==1 && air.data_len==5);
    p[0]=2; assert(send(0x200a,p,1)==0x12);
    /* Deferred stop is non-blocking, response arrives after quiescence. */
    before=responses; raw[0]=10; raw[1]=32; raw[2]=1; raw[3]=0; stop_result=0;
    assert(!ble_hci_send(1,raw,4)); ble_worker(); assert(responses==before && stops==1);
    ble_worker(); assert(responses==before); stop_result=1; ble_worker(); assert(responses==before+1 && !event[5]);
    p[0]=0; assert(!send(0x200a,p,1));
    /* Resumable advertising preparation defers completion and preserves
     * command FIFO, including a Reset arriving before publication. */
    before=responses; start_steps=3;
    assert(!ble_hci_send(1,(uint8_t[]){10,32,1,1},4)); ble_worker();
    assert(responses==before); ble_get_diag(&d); assert(!d.enabled);
    assert(!ble_hci_send(1,(uint8_t[]){3,12,0},3));
    ble_worker(); ble_worker(); assert(responses==before);
    ble_worker(); assert(responses==before+1 && !event[5] && ble_get16(event+3)==0x200a);
    ble_worker(); assert(responses==before+1); /* Reset begins its deferred stop. */
    for(i=0;i<64 && responses==before+1;i++) ble_worker();
    assert(responses==before+2 && !event[5] && ble_get16(event+3)==0x0c03);
    before=responses; start_steps=1;
    assert(!ble_hci_send(1,(uint8_t[]){10,32,1,1},4)); ble_worker(); start_failure=1; ble_worker();
    assert(responses==before+1 && event[5]==3); start_failure=0;
    /* Malformed envelopes are rejected at intake, never a completion. */
    before=responses;
    for (i=0;i<3;i++) assert(ble_hci_send(1,raw,(uint16_t)i));
    assert(ble_hci_send(2,raw,4) && ble_hci_send(3,raw,4) && ble_hci_send(1,0,3));
    raw[2]=252; assert(ble_hci_send(1,raw,255)); assert(responses==before);
    raw[0]=0xff; raw[1]=0xff; raw[2]=251;
    assert(!ble_hci_send(1,raw,254)); ble_worker(); assert(event[5]==1);
    raw[0]=1; raw[1]=16; raw[2]=1; raw[3]=0; assert(!ble_hci_send(1,raw,4)); ble_worker(); assert(event[5]==0x12);
    /* Command flood: four copied, ninety-six immediate, exactly 100 replies. */
    raw[0]=0xff; raw[1]=0xff; raw[2]=0; before=responses;
    for (i=0;i<100;i++) assert(!ble_hci_send(1,raw,3));
    assert(responses==before);
    for (i=0;i<100;i++) ble_worker();
    assert(responses==before+100);
    ble_get_diag(&d); assert(d.overflow==96);
    /* Reset cancels queued work and still answers the accepted command. */
    raw[0]=3; raw[1]=12; assert(!ble_hci_send(1,raw,3)); raw[0]=1; raw[1]=16;
    assert(!ble_hci_send(1,raw,3)); before=responses;
    for(i=0;i<64 && responses<before+2;i++) ble_worker();
    assert(responses==before+2 && !event[5]);
    assert(!send(0x2008,(uint8_t[32]){0},32));
    /* No public identity: public enable rejected, static random accepted. */
    ble_init((uint8_t[6]){0},host); p[0]=1; assert(send(0x200a,p,1)==0x12);
    assert(!send(0x1009,0,0) && !memcmp(event+6,(uint8_t[6]){0},6));
    assert(send(0x2005,(uint8_t[6]){0},6)==0x12);
    assert(!send(0x2005,(uint8_t[6]){1,2,3,4,5,0xc6},6));
    memset(p,0,15); ble_put16(p,160); ble_put16(p+2,160); p[5]=1; p[13]=7;
    assert(!send(0x2006,p,15)); p[0]=1; assert(!send(0x200a,p,1));
    assert(air.params.own_type==1 && air.addr[5]==0xc6);
    /* The internal driver also provisions the configured static random address. */
    ble_init((uint8_t[6]){0},host);
    ble_driver_start(0,(uint8_t[6]){1,2,3,4,5,0xc6});
    for (i=0;i<80;i++) ble_worker();
    ble_get_diag(&d); assert(!d.driver_error && d.enabled && air.params.own_type==1);
    /* Hardware failure is visible; no untrimmed advertising. */
    ble_init(pub,host); failure=1; assert(send(0x200a,p,1)==3); failure=0;
    /* Both internal profiles traverse the real H4 dispatcher. */
    ble_init(pub,host); ble_driver_start(0,0);
    for (i=0;i<80;i++) ble_worker();
    ble_get_diag(&d);
    assert(!d.driver_error && d.enabled && d.driver_step==14 && air.params.type==0 && air.scan_len==13);
    assert(!memcmp(air.scan+2,"Felucca BLE",11));
    ble_driver_start(3,0); for (i=0;i<80;i++) ble_worker(); ble_get_diag(&d);
    assert(!d.driver_error && d.enabled && air.params.type==3 && air.data_len==16);
    /* Atomic trace freeze, wrap, and no reference to overwritten host input. */
    ble_set_host(host); for (i=0;i<300;i++) assert(!send(0x1001,0,0));
    assert(ble_trace_size()==512); ble_trace_freeze(1);
    { const ble_trace_t *r=ble_trace_record(511); ble_trace_t saved=*r;
      assert(r && r->dir_type==4 && r->data[0]==14);
      assert(!send(0x1001,0,0)); assert(!memcmp(&saved,r,sizeof saved)); }
    ble_get_diag(&d); assert(d.trace_frozen_drops==2 && !ble_trace_record(512)); ble_trace_freeze(0);
    { static const uint16_t unknown[]={0x0405,0x0401,0x041f,0x0c63,0x1004,0x1804,
        0x200b,0x200c,0x200d,0x200e,0x200f,0x2010,0x2011,0x2012,0x2013,0x2014,
        0x2019,0x201d,0x201e,0x201f};
      for (i=0;i<sizeof unknown/sizeof unknown[0];i++) assert(send(unknown[i],0,0)==1); }
    puts("BLE S1: CRC, records, identity, channel/frame/PDU, H4 validation, commands, queue, stop/reset, driver, trace PASS");
}
int main(void) { logic_tests(); controller_tests(); return 0; }
