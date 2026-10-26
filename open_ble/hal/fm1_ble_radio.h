/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wall"
#pragma clang diagnostic error "-Wextra"
/* Full 24 MHz recipe, frozen r2 01. Included once by Felucca's HAL unit.
 * Every port poll is bounded. All waits use TIMER4; never alter the system PLL,
 * PMU regulators, WLA_CON0 bit14, or persistent items. */
#pragma once
#include "../ble_s1.h"
#include "../ble_memory.h"
#define BR_R(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define BR_A(n) BR_R(0x11900u+4u*(n))
#define BR_COUNT(a) (sizeof(a)/sizeof((a)[0]))
typedef struct { uint32_t a,b; } br_pair_t;
typedef struct { uint8_t a,b; } br_byte_pair_t;
typedef struct { uint8_t b[4]; } br_vga_t;
#include "fm1_ble_tables.h"
static ble_hw_diag_t br_diag;
static uint8_t br_limits[48],br_dc[16];
static int32_t br_txcmp[5];
static uint32_t br_mark,br_last_feed;
static void br_feed(void)
{
    uint32_t now=fm1_ticks();
    if ((uint32_t)(now-br_last_feed)>=24000000u) {
        fm1_wdt_feed(); br_last_feed=now;
    }
}

static inline void br_sync(void) { __asm__ volatile("csync" ::: "memory"); }
static inline void br_ssync(void) { __asm__ volatile("ssync" ::: "memory"); }
static void br_field(uint32_t addr,uint32_t mask,uint32_t value)
{
    if (!br_diag.error) BR_R(addr)=(BR_R(addr)&~mask)|(value&mask);
}
static void br_af(unsigned n,uint32_t mask,uint32_t value) { br_field(0x11900u+4u*n,mask,value); }
static void br_error(uint32_t code)
{
    if (!br_diag.error) { br_diag.error=code; br_diag.failure_stage=br_diag.stage; }
}
static int br_wait(uint32_t addr,uint32_t mask,uint32_t expected)
{
    uint32_t t=fm1_ticks();
    if (br_diag.error) return -1;
    while ((BR_R(addr)&mask)!=expected) {
        if ((uint32_t)(fm1_ticks()-t)>=24000u) { br_error(1); return -1; }
    }
    return 0;
}
static void br_stage(unsigned n)
{
    uint32_t t=fm1_ticks();
    BLE_DIAG_ONLY(if (br_diag.stage) br_diag.step_ticks[br_diag.stage-1]+=t-br_mark);
    (void)t; br_mark=t; br_diag.stage=n;
    fm1_wdt_feed();
}
static void br_phy_write(unsigned r,uint8_t data)
{
    uint32_t v=(r<<8)|data;
    br_feed();
    if (br_wait(0x3101cu,1u<<17,0)) return;
    BR_R(0x3101cu)=v|0x80000u; BR_R(0x3101cu)=v|0xa0000u;
}
static uint8_t br_phy_read(unsigned r)
{
    if (br_diag.error) return 0;
    BR_R(0x3101cu)=(r<<8)|0x90000u;
    BR_R(0x3101cu)=(r<<8)|0xb0000u; br_sync();
    return (uint8_t)BR_R(0x3101cu);
}
static void br_bank_write(unsigned bank,unsigned r,uint8_t value)
{
    br_phy_write(bank+1,(uint8_t)r); br_phy_write(bank+2,value); br_phy_write(bank,1);
}
static uint8_t br_bank_read(unsigned bank,unsigned r)
{
    br_phy_write(bank+1,(uint8_t)r); br_phy_write(bank,2); return br_phy_read(bank+3);
}
static void br_bank_field(unsigned bank,unsigned r,uint8_t mask,uint8_t value)
{
    uint8_t old=br_bank_read(bank,r);
    br_bank_write(bank,r,(uint8_t)((old&~mask)|(value&mask)));
}
static void br_image(unsigned bank,const br_byte_pair_t *image,unsigned n)
{
    unsigned i;
    for (i=0;i<n && !br_diag.error;i++) br_bank_write(bank,image[i].a,image[i].b);
}
static void br_phy_reset(void)
{
    if (br_diag.error) return;
    BR_R(0x31004u)=12; br_phy_write(22,0); br_phy_write(21,0x31);
    br_phy_write(21,0x30); br_phy_write(25,0);
}
static void br_mac_reset(void)
{
    if (br_diag.error) return;
    BR_R(0x31004u)=3; BR_R(0x3030cu)=30; BR_R(0x3030cu)=0; br_phy_reset();
}
static void br_sifs(void)
{
    static const br_pair_t image[]={
        {0x31104,0x00140314},{0x31100,0x33a40c0b},{0x31330,0x00040804},
        {0x31334,0x000c0808},{0x31338,0x000c0104},{0x31348,0x000f1290}};
    unsigned i;
    if (br_diag.error) return;
    for (i=0;i<BR_COUNT(image);i++) BR_R(image[i].a)=image[i].b;
}
static void br_digital_agc(void)
{
    unsigned i;
    for (i=0;i<128 && !br_diag.error;i++) {
        uint32_t w=br_dagc[i];
        int v=(int8_t)w;
        if (v< -64) v= -64;
        if (v>63) v=63;
        br_bank_write(0xc8,0x2f,(uint8_t)i);
        br_bank_write(0xc8,0x2b,(uint8_t)v); br_bank_write(0xc8,0x4e,(uint8_t)(w>>8));
        br_bank_write(0xc8,0x54,(uint8_t)(w>>16)); br_bank_write(0xc8,0x55,(uint8_t)(w>>24));
        br_bank_write(0xc8,0x2f,(uint8_t)(i|128u));
    }
    br_bank_write(0xc8,0x2f,0); br_bank_write(0xc8,0x54,0xc0); br_bank_write(0xc8,0x55,1);
    br_bank_write(0xc8,0x4e,0x42); br_bank_write(0xc8,0x2b,0x42); br_bank_write(0xc8,0x2c,0x42);
    br_bank_write(0xc8,0x79,0); br_bank_write(0xc8,0x7a,0x20);
}
static void br_phy_image(void)
{
    if (br_diag.error) return;
    br_phy_reset(); br_sifs(); BR_R(0x30308u)=0x9407;
    br_digital_agc();
    br_image(0xc8,br_c8,BR_COUNT(br_c8)); br_image(0xd4,br_d4,BR_COUNT(br_d4));
    br_image(0xcc,br_cc,BR_COUNT(br_cc)); br_image(0xd0,br_d0,BR_COUNT(br_d0));
    br_phy_write(106,4); br_phy_write(84,2); br_phy_reset();
}
static void br_linearise(void)
{
    unsigned i;
    br_bank_field(0xd0,81,3,0);
    for (i=0;i<128 && !br_diag.error;i++) {
        uint16_t v=br_rapp[i];
        br_bank_field(0xd0,92,16,16); br_bank_field(0xd0,93,127,(uint8_t)i);
        br_bank_write(0xd0,94,(uint8_t)v); br_bank_field(0xd0,92,32,(uint8_t)((v>>3)&32));
        br_bank_field(0xd0,92,16,0);
    }
    br_bank_write(0xd0,89,0xa0); br_bank_field(0xd0,88,7,0);
    br_bank_write(0xd0,82,100); br_bank_write(0xd0,83,45); br_bank_field(0xd0,81,12,12);
    br_bank_write(0xd0,90,0xa0); br_bank_field(0xd0,88,56,0);
    br_bank_write(0xd0,84,100); br_bank_write(0xd0,85,45); br_bank_field(0xd0,81,48,48);
    br_bank_write(0xd0,91,0x90); br_bank_field(0xd0,88,192,64); br_bank_field(0xd0,92,1,0);
    br_bank_write(0xd0,86,0xd6); br_bank_write(0xd0,87,63); br_bank_field(0xd0,81,192,128);
    br_bank_field(0xd0,81,3,2);
}
static void br_mode(unsigned mode)
{
    /* flags: RX=1 TX=2 TXcal=4 RXcal=8 RXDC=16 */
    static const struct { uint8_t reg,flags; uint32_t mask; } fields[]={
        {0,29,1u<<23},{1,1,1u<<15},{3,25,(1u<<4)|(1u<<5)|(1u<<20)},
        {4,29,(1u<<12)|(1u<<27)},{4,4,1u<<13},{5,29,(1u<<11)|(1u<<18)|(1u<<26)},
        {6,29,3},{2,14,1u<<6},{2,8,1u<<29},{6,14,1u<<19},{7,14,17},
        {8,14,1u<<9},{9,14,(1u<<5)|(1u<<7)|(1u<<8)|(1u<<27)},
        {10,14,(1u<<12)|(1u<<26)},{11,8,1u<<11},{11,4,1u<<14},
        {14,25,1u<<26},{14,14,1u<<27}};
    unsigned i,flag=mode<5 ? 1u<<mode : 0;
    for (i=0;i<BR_COUNT(fields);i++)
        br_af(fields[i].reg,fields[i].mask,(fields[i].flags&flag) ? fields[i].mask : 0);
}
static void br_rn(void)
{
    if (br_diag.error) return;
    br_af(14,(1u<<16)|(1u<<18),0); br_af(13,1u<<24,0); fm1_delay_us(5);
    br_af(14,1u<<16,1u<<16); fm1_delay_us(5);
    br_af(13,1u<<24,1u<<24); br_af(14,1u<<18,1u<<18); fm1_delay_us(100);
}
static uint32_t br_comparator(void)
{
    unsigned i;
    if (br_diag.error) return 0;
    br_af(26,0xf0000000u,0x10000000u);
    for (i=0;i<8;i++) BR_A(30)=1;
    BR_A(30)=0; br_sync(); return BR_A(30);
}
static unsigned br_compare(void)
{
    uint32_t r=br_comparator(); return ((r>>18)&1u)|((r>>16)&2u);
}
static uint8_t br_bank_for(unsigned f)
{
    unsigned i;
    for (i=1;i<48;i++) if (br_limits[i]>f) return (uint8_t)(br_limits[47]-(i-1));
    return (uint8_t)(br_limits[47]-47);
}
static void br_vco(void)
{
    unsigned c,i,kept=0,lo=73,hi_prev=73,found=0;
    br_af(16,1u<<24,1u<<24); br_af(15,255u<<5,73u<<5);
    for (c=7;c<=119 && !br_diag.error;c+=16) {
        unsigned m,l,h;
        br_af(14,127u<<19,c<<19); br_rn(); m=br_compare();
        br_af(14,127u<<19,(c-7)<<19); br_rn(); l=br_compare();
        br_af(14,127u<<19,(c+8)<<19); br_rn(); h=br_compare();
        kept=m;
        if (m!=l || m!=h) { kept=c; found=1; break; }
    }
    /* A degenerate detector is not a usable calibration (r2 leaves this unchecked). */
    if (!found) { br_error(2); return; }
    br_limits[47]=(uint8_t)(kept|15u);
    for (i=0;i<47 && !br_diag.error;i++) {
        unsigned f=lo,hi=171;
        br_af(14,127u<<19,((uint8_t)(br_limits[47]-i)&127u)<<19);
        for (;f<=171;f++) {
            unsigned r;
            br_af(15,255u<<5,f<<5); br_rn(); r=br_compare();
            if (r==1) { hi=f; break; }
            if (r==2) { lo=f; if (f==171) lo=171; }
        }
        br_limits[i]=(uint8_t)(((int)hi_prev-3+(int)lo)/2); hi_prev=hi;
        fm1_wdt_feed();
    }
    br_af(15,255u<<5,121u<<5); br_af(14,127u<<19,(br_bank_for(121)&127u)<<19);
    br_af(16,1u<<24,0); BLE_DIAG_ONLY(memcpy(br_diag.vco,br_limits,48));
}
static void br_gain(unsigned code)
{
    const uint8_t *b=br_vga[code<42 ? code : 41].b;
    br_af(4,31u<<22,((b[0]&7u)|((b[1]&3u)<<3))<<22);
    br_af(5,(7u<<15)|(7u<<12)|(7u<<5),((b[2]&7u)<<15)|((b[2]&0x70u)<<8)|((b[3]&7u)<<5));
}
static void br_tx(unsigned field,int value)
{
    unsigned packed=field<4 ? 97 : 98,shift=(field<4 ? field : field-4)*2;
    uint16_t v=(uint16_t)value&1023u;
    br_bank_write(0xd0,4+field,(uint8_t)(v>>2));
    br_bank_field(0xd0,packed,(uint8_t)(3u<<shift),(uint8_t)((v&3u)<<shift));
}
static uint16_t br_analog_dc(unsigned q,int v)
{
    uint32_t magnitude=v<0 ? 0u-(uint32_t)v : (uint32_t)v;
    uint16_t code=(uint16_t)((magnitude&511u)|(v>=0 ? 512u : 0u));
    unsigned shift=q ? 17 : 7;
    br_af(2,1023u<<shift,((uint32_t)code&1023u)<<shift); return code;
}
static void br_apply_trim(const uint8_t *t)
{
    unsigned i;
    br_af(7,127u<<8,(uint32_t)t[24]<<8); br_af(8,127,t[25]);
    for (i=0;i<6;i++) br_tx(i,(int32_t)ble_get32(t+4*i));
    br_analog_dc(0,(int32_t)ble_get32(t+32)); br_analog_dc(1,(int32_t)ble_get32(t+36));
    br_bank_write(0xd4,9,t[26]); br_bank_write(0xd4,10,t[28]);
    br_bank_write(0xd4,11,t[27]); br_bank_write(0xd4,12,t[29]);
    if (br_diag.error) return;
    BR_A(23)=ble_get32(t+40); BR_A(24)=ble_get32(t+44);
    br_af(1,15u<<8,8u<<8); br_af(8,255u<<10,(uint32_t)t[42]<<10); br_af(4,255u<<14,(uint32_t)t[42]<<14);
    memcpy(br_dc,t+48,16);
}
static void br_loopback(void) { br_phy_reset(); if (!br_diag.error) BR_R(0x31004u)=12; br_phy_write(25,24); }
static uint32_t br_measure(unsigned sel)
{
    uint32_t t=fm1_ticks();
    int32_t i,q;
    br_bank_write(0xd4,21,1); br_bank_write(0xd4,22,1);
    br_bank_write(0xd4,21,3); br_bank_write(0xd4,22,3);
    while (!br_diag.error && (br_bank_read(0xd4,0x8d)&3u)) {
        if ((uint32_t)(fm1_ticks()-t)>=24000u) { br_error(3); return 0; }
    }
    if (sel==1 || sel==2) {
        unsigned r=sel==1 ? 0x81 : 0x8a;
        uint32_t a=br_bank_read(0xd4,r),b=br_bank_read(0xd4,r+1),c=br_bank_read(0xd4,r+2);
        return (uint32_t)((int32_t)((a<<24)|(b<<16)|(c<<8))/256);
    }
    i=(int16_t)((unsigned)br_bank_read(0xd4,0x81)<<8|br_bank_read(0xd4,0x82));
    q=(int16_t)((unsigned)br_bank_read(0xd4,0x84)<<8|br_bank_read(0xd4,0x85));
    return (uint32_t)(i*i)+(uint32_t)(q*q);
}
static void br_gain_step(void)
{
    unsigned g;
    for (g=0;g<=36 && !br_diag.error;g++) {
        uint8_t peak;
        br_gain(g); br_bank_write(0xd4,23,0x82); fm1_delay_us(1000);
        peak=br_bank_read(0xd4,0x80); br_bank_write(0xd4,23,2);
        if (peak>47) break;
    }
}
static void br_bandwidth(void)
{
    static const unsigned configs[3]={0x1fc,0x122,0xa0};
    uint32_t word=255;
    unsigned i;
    br_af(1,15u<<8,8u<<8); br_af(20,1u<<27,1u<<27);
    for (i=0;i<3 && !br_diag.error;i++) {
        uint32_t t=fm1_ticks(),r;
        br_af(26,0xffffffu,configs[i]|(30u<<10)); br_af(26,1,1);
        do {
            r=br_comparator();
            if ((uint32_t)(fm1_ticks()-t)>=24000u) { br_error(4); return; }
        } while (!(r&32u));
        word|=((BR_A(30)>>8)&255u)<<(8u*(i+1)); br_af(26,1,0);
    }
    br_af(20,1u<<27,0);
    if (br_diag.error) return;
    BR_A(23)=BR_A(24)=word;
    br_af(8,255u<<10,((word>>16)&255u)<<10); br_af(4,255u<<14,((word>>16)&255u)<<14);
}
static void br_atten(int rx)
{
    br_af(8,15u<<20,(rx ? 6u : 10u)<<20);
    br_af(9,7u<<28,5u<<28); br_af(10,(3u<<13)|(3u<<27),(3u<<13)|((rx ? 1u : 3u)<<27));
    br_af(4,15u<<8,9u<<8);
}
static void br_tx_search(void)
{
    unsigned axis;
    int k,best,flag=1;
    uint32_t metric;
    br_phy_reset(); br_bank_write(0xc8,0x15,0); br_bank_write(0xc8,0x14,0);
    br_bank_field(0xc8,0x40,128,0); br_bank_field(0xc8,0x41,128,0);
    br_bank_write(0xc8,0x28,0); br_bank_write(0xc8,0x27,5);
    br_phy_write(4,(uint8_t)(br_phy_read(4)&0xe7)); br_loopback();
    br_bank_write(0xd0,0,3); br_bank_write(0xd0,95,16);
    br_bank_write(0xd0,1,20); br_bank_write(0xd0,2,0); br_bank_write(0xd0,3,0);
    br_bank_write(0xd4,0,62); br_bank_write(0xd4,19,30);
    br_bank_write(0xd4,20,56); br_bank_write(0xd4,20,52);
    br_tx(0,1023); br_tx(1,0); br_tx(2,1023); br_tx(3,0); br_tx(4,0); br_tx(5,0);
    br_af(7,127u<<8,64u<<8); br_af(8,127,64); br_gain_step();
    for (axis=0;axis<2 && !br_diag.error;axis++) {
        unsigned reg=axis ? 8 : 7,shift=axis ? 0 : 8;
        br_af(reg,127u<<shift,64u<<shift); best=64; metric=br_measure(0);
        for (k=0;k<=127 && !br_diag.error;k++) {
            uint32_t m;
            br_af(reg,127u<<shift,(uint32_t)k<<shift); m=br_measure(0);
            if (m<metric) { metric=m; best=k; }
        }
        br_af(reg,127u<<shift,(uint32_t)best<<shift);
    }
    br_tx(4,0); br_tx(5,0); br_gain_step();
    for (axis=0;axis<2 && !br_diag.error;axis++) {
        best=0; metric=br_measure(0);
        for (k=-200;k<=200 && !br_diag.error;k++) {
            uint32_t m;
            br_tx(4+axis,(int8_t)k); m=br_measure(0);
            if (m<metric) { metric=m; best=(int8_t)k; }
        }
        br_tx(4+axis,best);
    }
    br_bank_write(0xd4,19,60); br_tx(0,1023); br_tx(2,1023);
    best=0; metric=br_measure(0);
    for (k=-423;k<=423 && !br_diag.error;k++) {
        uint32_t m;
        br_tx(0,k<0 ? 1023+k : 1023); br_tx(2,k<0 ? 1023 : 1023-k); m=br_measure(0);
        if (m<metric) { metric=m; best=(int8_t)k; flag=k>=0; }
    }
    br_tx(0,flag ? 1023 : 1023+best); br_tx(2,flag ? 1023-best : 1023);
    br_tx(1,0); br_tx(3,0); best=0; metric=br_measure(0);
    for (k=-200;k<=200 && !br_diag.error;k++) {
        uint32_t m;
        br_tx(flag ? 3 : 1,k); m=br_measure(0);
        if (m<metric) { metric=m; best=k; }
    }
    br_tx(flag ? 3 : 1,best);
}
static void br_rx_dc_search(void)
{
    static const uint8_t groups[4][3]={{0,0,127},{1,0,127},{1,3,69},{1,9,14}};
    unsigned g,j,axis;
    br_phy_reset(); br_loopback(); br_bank_write(0xd0,0,3); br_bank_write(0xd0,95,16);
    br_bank_write(0xd4,0,62); br_bank_write(0xd4,27,56); br_bank_write(0xd4,8,0xc0);
    br_bank_write(0xd4,19,0); br_bank_write(0xd4,20,56); br_bank_write(0xd4,20,52); br_gain(40);
    for (g=0;g<4 && !br_diag.error;g++) {
        int dc[2]={0,0},step=256;
        br_af(3,3u<<6,(uint32_t)groups[g][0]<<6);
        br_af(4,(15u<<8)|127u,((uint32_t)groups[g][1]<<8)|groups[g][2]);
        br_analog_dc(0,0); br_analog_dc(1,0);
        for (j=0;j<12 && !br_diag.error;j++) {
            for (axis=0;axis<2;axis++) {
                int32_t m=(int32_t)br_measure(axis+1);
                if (m>0 && dc[axis]>-510) dc[axis]-=step;
                else if (m<0 && dc[axis]<510) dc[axis]+=step;
                if (dc[axis]<-510) dc[axis]=-510;
                if (dc[axis]>510) dc[axis]=510;
                br_analog_dc(axis,dc[axis]);
            }
            if (step>1) step/=2;
        }
        for (axis=0;axis<2;axis++) ble_put16(br_dc+4*g+2*axis,br_analog_dc(axis,dc[axis]));
    }
}
static void br_rx_search(void)
{
    int k,best=0,flag=1;
    uint32_t metric;
    br_phy_reset(); br_loopback(); br_bank_write(0xd0,0,3); br_bank_write(0xd0,95,16);
    br_bank_write(0xd0,1,20); br_bank_write(0xd0,2,0); br_bank_write(0xd0,3,17);
    br_bank_write(0xd4,9,255); br_bank_write(0xd4,11,255);
    br_bank_write(0xd4,10,0); br_bank_write(0xd4,12,0);
    br_bank_write(0xd4,0,62); br_bank_write(0xd4,20,56); br_bank_write(0xd4,20,4);
    br_bank_write(0xd4,19,30); br_gain_step(); metric=br_measure(3);
    for (k=-105;k<=105 && !br_diag.error;k++) {
        uint32_t m;
        br_bank_write(0xd4,9,(uint8_t)(k<0 ? 255+k : 255));
        br_bank_write(0xd4,11,(uint8_t)(k<0 ? 255 : 255-k)); m=br_measure(3);
        if (m<metric) { metric=m; best=(int8_t)k; }
    }
    flag=best>=0;
    br_bank_write(0xd4,9,(uint8_t)(flag ? 255 : best-1));
    br_bank_write(0xd4,11,(uint8_t)(flag ? ~best : 255));
    best=0; metric=br_measure(3);
    for (k=-50;k<=50 && !br_diag.error;k++) {
        uint32_t m;
        br_bank_write(0xd4,flag ? 12 : 10,(uint8_t)k); m=br_measure(3);
        if (m<metric) { metric=m; best=k; }
    }
    br_bank_write(0xd4,flag ? 12 : 10,(uint8_t)best);
}
static void br_self_calibrate(void)
{
    br_bandwidth(); br_mode(2); br_atten(0); br_tx_search();
    br_af(5,31,0); br_mode(0); br_af(1,1u<<15,0); br_rx_dc_search();
    br_af(5,31,25); br_mode(3); br_atten(1); br_rx_search();
}
static int br_signed10(unsigned v) { return (v&512u) ? (int)v-1024 : (int)v; }
static void br_handover(void)
{
    unsigned r4=br_bank_read(0xd0,4),r6=br_bank_read(0xd0,6);
    unsigned r5=br_bank_read(0xd0,5),r7=br_bank_read(0xd0,7);
    unsigned r8=br_bank_read(0xd0,8),r9=br_bank_read(0xd0,9);
    unsigned p=br_bank_read(0xd0,97),d=br_bank_read(0xd0,98);
    br_txcmp[0]=(int32_t)(r4>>1); br_txcmp[1]=(int32_t)(r6>>1);
    br_txcmp[2]=(br_signed10((r5<<2)|((p>>2)&3u))+br_signed10((r7<<2)|((p>>6)&3u)))/8;
    br_txcmp[3]=br_signed10((r8<<2)|(d&3u))/4;
    br_txcmp[4]=br_signed10((r9<<2)|((d>>2)&3u))/4;
}
static int br_seq_wait(void)
{
    uint32_t t=fm1_ticks();
    if (br_diag.error) return -1;
    while (BR_R(0x14028u)&0x700000u) {
        BR_R(0x14034u)=0;
        if ((uint32_t)(fm1_ticks()-t)>=24000u) { br_error(5); return -1; }
    }
    return 0;
}
static void br_strobe(void)
{
    unsigned i;
    for (i=0;i<5;i++) BR_R(0x14028u)|=16;
    for (i=0;i<5;i++) BR_R(0x14028u)&=~16u;
}
static void br_sequencer(const uint8_t *pa)
{
    unsigned i;
    if (br_diag.error) return;
    BR_R(0x14028u)=0; BR_R(0x1402cu)=0; BR_R(0x14030u)=0; BR_R(0x14028u)=0x10000;
    if (br_seq_wait()) return;
    BR_R(0x1402cu)=0; BR_R(0x14030u)=0; BR_R(0x14028u)=0x10000; br_strobe();
    for (i=0;i<256 && !br_diag.error;i++) {
        uint32_t a=br_seq[i].a,b=br_seq[i].b;
        if (i<128) b|=(uint32_t)br_bank_for((b>>21)&255u)<<9;
        else if (i<224) {
            unsigned g=i<133 ? 0 : i<180 ? 1 : i<186 ? 2 : 3;
            a=(a&4095u)|((uint32_t)(ble_get16(br_dc+4*g)&1023u)<<12)|
                ((uint32_t)(ble_get16(br_dc+4*g+2)&1023u)<<22);
        } else {
            a=(a&~63u)|(pa[5]&7u)|((pa[6]&7u)<<3);
            b=(b&~((7u<<9)|(7u<<15)|(7u<<18)|(7u<<23)|(15u<<26)))|
                ((pa[0]&7u)<<9)|((pa[1]&7u)<<15)|((pa[2]&7u)<<18)|
                ((pa[3]&7u)<<23)|((pa[4]&15u)<<26);
        }
        if (br_seq_wait()) return;
        BR_R(0x1402cu)=a; BR_R(0x14028u)=(i<<8)|0x1000e; br_strobe();
        if (br_seq_wait()) return;
        BR_R(0x1402cu)=b; BR_R(0x14028u)=(i<<8)|0x1000d; br_strobe();
    }
    if (br_seq_wait()) return;
    for (i=0;i<6;i++) BR_R(0x14028u)=0;
    BR_R(0x14028u)=0x10000;
}
static inline void br_p33_snapshot(uint8_t *out)
{
    unsigned i,n=0;
    for (i=0;i<=0x17;i++) out[n++]=fm1_p33_read(i);
    for (i=0x30;i<=0x35;i++) out[n++]=fm1_p33_read(i);
    for (i=0x70;i<=0x77;i++) out[n++]=fm1_p33_read(i);
    br_diag.p33_timeouts=fm1_p33_timeouts;
}
static void br_clock_probe(void)
{
    uint32_t n=100000,t=fm1_ticks(),dt;
    /* Unknown cycle cost/cache state: this measures this loop's throughput,
     * not CPU Hz. Every radio wait uses TIMER4 independently (r2b 04 §0.4). */
    do { __asm__ volatile("nop"); } while (--n);
    dt=fm1_ticks()-t; br_diag.loop_ticks=dt;
    if (!dt) br_error(10);
    else br_diag.loop_iters_per_us_x1000=2400000000u/dt;
    BLE_DIAG_ONLY(br_diag.clocks[0]=BR_R(0x1000cu); br_diag.clocks[1]=BR_R(0x10010u););
    BLE_DIAG_ONLY(br_diag.clocks[2]=BR_R(0x10014u); br_diag.clocks[3]=BR_R(0x10008u); /* SYS_DIV, public WL82.h */);
}
static void br_collect_trim(uint8_t *out)
{
    unsigned i;
    uint32_t dc;
    memset(out,0,68);
    for (i=0;i<6;i++) {
        unsigned packed=br_bank_read(0xd0,i<4 ? 97 : 98),shift=(i<4 ? i : i-4)*2;
        unsigned value=((unsigned)br_bank_read(0xd0,4+i)<<2)|((packed>>shift)&3u);
        int v=(i==0 || i==2) ? (int)value : br_signed10(value);
        ble_put32(out+4*i,(uint32_t)v);
    }
    out[24]=(uint8_t)((BR_A(7)>>8)&127u); out[25]=(uint8_t)(BR_A(8)&127u);
    out[26]=br_bank_read(0xd4,9); out[27]=br_bank_read(0xd4,11);
    out[28]=br_bank_read(0xd4,10); out[29]=br_bank_read(0xd4,12);
    dc=BR_A(2);
    for (i=0;i<2;i++) {
        unsigned v=(dc>>(i ? 17 : 7))&1023u;
        int value=(v&512u) ? (int)(v&511u) : -(int)(v&511u);
        ble_put32(out+32+4*i,(uint32_t)value);
    }
    ble_put32(out+40,BR_A(23)); ble_put32(out+44,BR_A(24)); memcpy(out+48,br_dc,16);
    ble_put32(out+64,ble_crc16(0,out,64));
}
static void br_radio_init(ble_cal_t *cal,uint32_t *modem)
{
    static const struct { uint8_t n; uint32_t mask,value; } analog[]={
        {0,0xfffc3fff,0xea200005},{1,0xffffffff,0x01c35880},{2,0xffffffff,0x9c010010},
        {3,0xffffffff,0x672b9349},{4,0x3fffffff,0x0508098e},{5,0x7fffffff,0x4078c319},
        {6,0x000fffff,0x00076a24},{7,0xffffffff,0x2915c066},{8,0x3fffffff,0x00a641c0},
        {9,0x7fffffff,0x65b80057},{10,0x1fffffff,0x1afae7e7},{11,0x007fffff,0x0052a070},
        {13,0x3fffffff,0x01000000},{14,0xffffffff,0x819bd033},{15,0x7fffffff,0x3a780ef2},
        {16,0x3fffffff,0x08b41490},{17,0x000fffff,0x00000a80},{25,0x00030000,0x00010000}};
    static const uint8_t mcs_regs[12]={11,13,15,17,27,29,31,33,19,21,23,25};
    static const uint8_t mcs_idx[12]={0,1,2,3,5,7,9,11,12,14,16,19};
    static const uint8_t mcs_default[12]={0x20,0x20,0x20,0x20,0x20,0x2b,0x26,0x20,0x20,0x2b,0x26,0x20};
    unsigned i;
    uint32_t start=fm1_ticks();
    BLE_DIAG_ONLY(br_diag.clocks[0]=BR_R(0x1000cu); br_diag.clocks[1]=BR_R(0x10010u););
    BLE_DIAG_ONLY(br_diag.clocks[2]=BR_R(0x10014u); br_diag.clocks[3]=BR_R(0x10008u););
    BLE_DIAG_ONLY(br_p33_snapshot(br_diag.p33_before););
    br_stage(1);
    br_field(0x10010u,3u<<14,0);
    for (i=0;i<5;i++) BR_R(0x14040u+4*i)=0;
    br_field(0x14040u,1u<<8,1u<<8); br_field(0x14040u,1u<<6,1u<<6);
    br_field(0x14040u,1,1); fm1_delay_us(1); br_field(0x14040u,4,4); fm1_delay_us(1);
    br_field(0x14040u,8,8); fm1_delay_us(1); br_field(0x14040u,2,2); fm1_delay_us(50);
    br_field(0x1405cu,255,5); br_field(0x1405cu,0xffff00,0);
    br_field(0x1405cu,0xc7000000u,0x47000000u);
    br_stage(2);
    br_field(0x1404cu,1u<<24,1u<<24);
    { static const uint8_t bits[]={11,10,9,5,4,3,2,1,0};
      for (i=0;i<BR_COUNT(bits);i++) br_field(0x30f04u,1u<<bits[i],bits[i]==3 ? 0 : 1u<<bits[i]); }
    br_field(0x30f00u,1u<<10,1u<<10); br_field(0x30f00u,3u<<8,0);
    br_field(0x30f00u,7u<<5,3u<<5); br_field(0x30f00u,16,0);
    for (i=4;i>0;i--) br_field(0x30f00u,1u<<(i-1),1u<<(i-1));
    br_stage(3);
    br_mac_reset(); br_sifs(); if (!br_diag.error) BR_R(0x30308u)=0x9407;
    br_mac_reset(); br_image(0xd0,br_cfr_a,BR_COUNT(br_cfr_a)); br_phy_image();
    br_stage(4);
    br_af(12,0x0fffffffu,0x0580025fu|((cal->xtal[0]&15u)<<13)|((cal->xtal[1]&15u)<<19));
    if (!br_diag.error) BR_A(20)=0;
    for (i=0;i<BR_COUNT(analog);i++) br_af(analog[i].n,analog[i].mask,analog[i].value);
    br_stage(5); br_mode(5); br_rn(); br_vco();
    br_af(13,0x0effffffu,0x00c00000u); br_af(14,4095,101);
    br_af(16,1u| (3u<<6),1); br_af(15,(15u<<18)|(7u<<27),(9u<<18)|(2u<<27));
    if (!br_diag.error) BR_A(21)=0x02822520;
    br_rn(); br_af(14,1u<<18,0); fm1_delay_us(80); br_af(14,1u<<18,1u<<18);
    br_stage(6);
    if (!br_diag.error) {
        if ((cal->present&8u) && ble_trim_valid(cal->trim)) {
            br_diag.calibration=1; br_apply_trim(cal->trim);
        } else { br_diag.calibration=2; br_self_calibrate(); }
    }
    br_stage(7); br_mode(0);
    br_af(9,7u<<24,(cal->pa[0]&7u)<<24); br_af(10,7,cal->pa[1]);
    br_af(10,7u<<9,(cal->pa[2]&7u)<<9); br_af(10,7u<<15,(cal->pa[3]&7u)<<15);
    br_af(10,15u<<22,(cal->pa[4]&15u)<<22); br_af(11,7,cal->pa[5]);
    br_stage(8); br_handover();
    if (br_diag.calibration==2 && !br_diag.error) br_collect_trim(cal->trim);
    br_stage(9);
    br_af(1,(3u<<16)|(7u<<12),(3u<<16)|(4u<<12)); br_af(2,7,0);
    br_af(3,3u<<6,1u<<6); br_af(4,(15u<<8)|127u,(9u<<8)|14u); br_gain(0);
    br_stage(10); br_phy_image();
    br_stage(11); br_image(0xd0,br_cfr_b,BR_COUNT(br_cfr_b)); br_linearise();
    br_stage(12); br_sequencer(cal->pa);
    br_stage(13);
    if (!br_diag.error) BR_A(20)=0x57bfffdc;
    br_af(9,15u<<5,15u<<5); br_af(14,3u<<26,3u<<26); br_af(25,0xff000000u,0x3b000000u);
    br_field(0x1405cu,0xc7000000u,0x47000000u); br_field(0x1405cu,0xff0000,0x60000);
    br_field(0x1405cu,255,0x6b);
    for (i=0;i<12;i++) br_bank_write(0xd0,mcs_regs[i],cal->mcs[mcs_idx[i]] ? cal->mcs[mcs_idx[i]] : mcs_default[i]);
    br_stage(14);
    /* Vendor loop240 = 5.3 us; conservative fixed 10 us, no CPU assumption. */
    br_field(0x10010u,3u<<14,0); fm1_delay_us(10);
    br_field(0x14000u,(1u<<7)|(31u<<16),(1u<<7)|(12u<<16)); fm1_delay_us(10);
    br_field(0x14000u,1,1); fm1_delay_us(10);
    br_field(0x2fc40u,1u<<10,1u<<10); br_field(0x20000u,3u<<30,3u<<30); fm1_delay_us(10);
    if (!br_diag.error) { BR_R(0x2fc40u)=65535; BR_R(0x2fc40u)&=~0x302u; BR_R(0x2fc78u)=15; BR_R(0x2fc78u)|=0x3f0; }
    br_stage(15);
    if (!br_diag.error) {
        BR_R(0x2fc88u)=(uint32_t)(uintptr_t)modem;
        for (i=0;i<81;i++) { modem[3*i]=(i<<8)|i; modem[3*i+1]=modem[3*i+2]=0; }
        br_sync(); BR_R(0x2fd98u)=0;
        for (i=0;i<64;i++) { BR_R(0x2fd9cu)=br_agc[i].a; BR_R(0x2fd9cu)=br_agc[i].b; }
        BR_R(0x2fc20u)&=~(3u<<30);
        BR_R(0x2fd80u)=0x000f0000; BR_R(0x2fd84u)=0x1872bf14; BR_R(0x2fd88u)=0x1872bf55;
        BR_R(0x2fd8cu)=0x1872bf00; BR_R(0x2fd90u)=0; BR_R(0x2fd94u)=0; fm1_delay_us(10);
        BR_R(0x2fd80u)|=1; BR_R(0x2fd80u)&=~(1u<<20); BR_R(0x2fc7cu)|=1;
        BR_R(0x2fc48u)=0xb9; BR_R(0x2fc00u)=0x00ffd144; BR_R(0x2fc04u)=0x4e143cdf;
        BR_R(0x2fc08u)=0x03100000; BR_R(0x2fc0cu)=0x80808080; BR_R(0x2fc10u)=0x80008080;
        BR_R(0x2fc14u)=128; BR_R(0x2fc18u)=0; BR_R(0x2fc1cu)=0;
        BR_R(0x2fc20u)=0x36555537; BR_R(0x2fc24u)=0x00034041; BR_R(0x2fc28u)=0x0005c000;
        br_field(0x2fc10u,0xffffffu,(uint32_t)br_txcmp[0]|((uint32_t)br_txcmp[1]<<8)|((uint32_t)(-br_txcmp[2])&255u)<<16);
        br_field(0x2fc08u,0xfffffu,((uint32_t)br_txcmp[3]&1023u)|(((uint32_t)br_txcmp[4]&1023u)<<10));
        br_field(0x2fc9cu,15,8); BR_R(0x2fc98u)=0x02360236;
        br_field(0x2fc9cu,0xffff0000u,0x36360000); fm1_delay_us(10); br_field(0x2fca0u,65535,0x3636);
    }
    br_stage(16); br_diag.bringup_ticks=fm1_ticks()-start;
    if (br_diag.error) {
        /* Before BB opens, request only the specified manual "all off" mode.
         * No undocumented abort/reset/clock gate; a bus hang may still defeat it. */
        uint32_t error=br_diag.error;
        br_diag.error=0; BR_A(20)=0; br_mode(5); br_sync(); br_diag.error=error;
    }
    BLE_DIAG_ONLY(br_p33_snapshot(br_diag.p33_after););
}

#pragma clang diagnostic pop
