/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* r2 01 §§2.2,4.7; 02 §§3,5.3,6.2; 03 §3.5; 04 §7. No MMIO. */
#include "ble_s1.h"
#include "ble_memory.h"

uint16_t ble_get16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
uint32_t ble_get32(const uint8_t *p) { return (uint32_t)ble_get16(p) | (uint32_t)ble_get16(p+2) << 16; }
void ble_put16(uint8_t *p, uint16_t n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); }
void ble_put32(uint8_t *p, uint32_t n) { ble_put16(p,(uint16_t)n); ble_put16(p+2,(uint16_t)(n>>16)); }

uint16_t ble_crc16(uint16_t seed, const uint8_t *p, uint32_t n)
{
    while (n--) {
        unsigned i;
        seed ^= (uint16_t)*p++ << 8;
        for (i=0; i<8; i++)
            seed = (uint16_t)((seed << 1) ^ ((seed & 0x8000u) ? 0x1021u : 0u));
    }
    return seed;
}
int ble_trim_valid(const uint8_t *p)
{
    uint32_t check=ble_get32(p+64);
    return check && check==ble_crc16(0,p,64);
}
static int uniform(const uint8_t *p, unsigned n, uint8_t v)
{
    while (n--) if (*p++ != v) return 0;
    return 1;
}
int ble_public_valid(const uint8_t *p)
{
    static const uint8_t placeholder[6]={1,2,3,4,5,8};
    return !uniform(p,6,0) && !uniform(p,6,255) && memcmp(p,placeholder,6);
}
int ble_random_valid(const uint8_t *p)
{
    uint8_t v[6];
    memcpy(v,p,6); v[5]&=63;
    return (p[5]&192)==192 && !uniform(v,6,0) &&
        !(uniform(v,5,255) && v[5]==63);
}
void ble_derive_public(const uint8_t *mac, uint8_t *addr)
{
    uint8_t b[2];
    uint16_t c=ble_crc16(0,mac,6), d;
    ble_put16(b,c); d=ble_crc16(c,b,2);
    memcpy(addr,mac,6); addr[0]=b[0]; addr[1]=b[1]; addr[2]=(uint8_t)(mac[2]+d);
}

/* S1 reads only the preserved first VM sector and BTIF. The r2 specification
 * does not define active-half selection after compaction. Never search Felucca's
 * store for apparent records. Status 1=end, 2=bad magic, 3=bad/truncated length,
 * 4=read failure. Invalid outer CRC records are skipped, last valid record wins. */
static uint8_t read_log(ble_flash_read_fn read, uint32_t off, uint32_t end, ble_cal_t *cal)
{
    uint8_t h[4], payload[68];
    while (end-off>=4) {
        uint16_t id, n, crc=0;
        uint32_t pos;
        if (read(off,h,4)) return 4;
        if (uniform(h,4,255)) return 1;
        id=(uint16_t)(h[1] | (h[2]&15u)<<8);
        n=(uint16_t)(ble_get16(h+2)>>4);
        if (!n || n>end-off-4) return 3;
        for (pos=0; pos<n;) {
            uint32_t k=n-pos;
            if (k>sizeof payload) k=sizeof payload;
            if (read(off+4+pos,payload,k)) return 4;
            crc=ble_crc16(crc,payload,k); pos+=k;
        }
        if ((uint8_t)crc==h[0] && n<=sizeof payload) {
            uint8_t *dst=0, bit=0;
            if (id==106 && n==2) dst=cal->xtal,bit=1;
            if (id==107 && n==7) dst=cal->pa,bit=2;
            if (id==108 && n==20) dst=cal->mcs,bit=4;
            if (id==187 && n==68) dst=cal->trim,bit=8;
            if (id==102 && n==6) dst=cal->public_addr,bit=16;
            if (dst) { memcpy(dst,payload,n); cal->present|=bit; }
        }
        off+=4u+n;
    }
    return 1;
}
void ble_read_cal(ble_flash_read_fn read, ble_cal_t *cal)
{
    uint8_t h[4], mac[6];
    static const uint8_t pa[7]={1,7,4,7,11,1,7};
    memset(cal,0,sizeof *cal);
    cal->xtal[0]=cal->xtal[1]=11; memcpy(cal->pa,pa,7);
    if (read(0x93000u,h,4)) cal->vm_status=4;
    else if (ble_get32(h)!=0x55aaaa55u) cal->vm_status=2;
    else cal->vm_status=read_log(read,0x93004u,0x94000u,cal);
    cal->btif_status=read_log(read,0xe9000u,0xea000u,cal);
    memcpy(mac,cal->public_addr,6);
    if ((cal->present&16u) && ble_public_valid(mac)) {
        ble_derive_public(mac,cal->public_addr);
        if (!ble_public_valid(cal->public_addr)) memset(cal->public_addr,0,6);
    } else memset(cal->public_addr,0,6);
}
uint8_t ble_adv_validate(const uint8_t *p, ble_adv_params_t *out)
{
    uint16_t min=ble_get16(p),max=ble_get16(p+2);
    if (min<32 || max>16384 || min>max || p[4]>4 || p[5]>3 || p[6]>1 ||
        !p[13] || (p[13]&~7u) || p[14]>3) return 0x12;
    if ((p[4]!=0 && p[4]!=3) || p[5]>1 || p[14]) return 0x11;
    out->min=min; out->max=max; out->type=p[4]; out->own_type=p[5];
    out->peer_type=p[6]; memcpy(out->peer,p+7,6); out->channels=p[13]; out->filter=p[14];
    return 0;
}
uint8_t ble_frequency(uint8_t ch)
{
    if (ch==37) return 0;
    if (ch==38) return 24;
    if (ch==39) return 78;
    return (uint8_t)(2u*(ch+1u)+(ch>=11 ? 2u : 0u));
}
uint16_t ble_adv_channel_word(uint8_t map)
{
    unsigned ch,first=0,second=0,n=0;
    for (ch=37;ch<=39;ch++) if (map&(1u<<(ch-37))) {
        if (!n) first=ch; else if (n==1) second=ch;
        n++;
    }
    return (uint16_t)(0x4000u | ((n>1 ? second-first : 0)<<8) |
        ((map&1u) ? 0u : 64u) | ((map&4u) ? 128u : 0u) | first);
}
uint16_t ble_adv_header(uint8_t type,uint8_t own_type)
{
    return (uint16_t)(type | (uint16_t)own_type<<4);
}
uint8_t ble_adv_payload(uint8_t *out,const uint8_t *addr,const uint8_t *data,uint8_t n)
{
    if (n>31) return 0;
    memcpy(out,addr,6); memcpy(out+6,data,n); return (uint8_t)(n+6);
}
void ble_frame_init(uint8_t *f,const ble_adv_t *adv)
{
    unsigned i,n=0;
    memset(f,0,BLE_FRAME_SIZE);
    for (i=0;i<3;i++) n+=(adv->params.channels>>i)&1u;
    ble_put16(f+BF_ADV_CFG,(uint16_t)((n<<14)|(adv->params.type==3 ? 625 : 1250)));
    ble_put16(f+BF_MODE,(uint16_t)(8u|(adv->params.type==3 ? 256u : 0u)));
    ble_put16(f+BF_OPTIONS,0x228); ble_put16(f+10,0xbed6); ble_put16(f+12,0x8e89);
    ble_put16(f+BF_TX_PTR,1); ble_put16(f+BF_TX_PTR+2,1);
    ble_put16(f+BF_TX_DATA,1); ble_put16(f+BF_TX_DATA+2,1);
    ble_put16(f+BF_WIN,50); ble_put16(f+BF_WIN_STEADY,50);
    ble_put16(f+BF_CHMAP,65535); ble_put16(f+BF_CHMAP+2,65535);
    ble_put16(f+BF_CHMAP+4,(37u<<5)|31u); ble_put16(f+BF_RX_MAX,255);
    ble_put16(f+BF_CRC,0x5555); ble_put16(f+BF_CRC+2,0x55);
    ble_put16(f+74,0x51e7); memcpy(f+BF_LOCAL,adv->addr,6);
    ble_put16(f+92,0x8295); ble_put16(f+BF_PRIO_CTRL,26); ble_put16(f+BF_PRIO_STAT,26);
    ble_put16(f+BF_FLOW,6);
    for (i=0;i<40;i++) {
        f[BF_FREQ_IDX+i]=f[BF_FREQ_USED+i]=(uint8_t)i;
        f[BF_FREQ_TBL+i]=f[BF_FREQ_USED_TBL+i]=ble_frequency((uint8_t)i);
    }
    ble_put32(f+BF_POWER,16u+adv->power); ble_put32(f+BF_TX_SET,16u+adv->power);
}
