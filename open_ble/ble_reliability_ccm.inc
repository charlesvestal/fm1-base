/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Independent, diagnostic CCM verifier. One TinyCrypt key expansion or AES
 * block per executor item; never holds IRQ exclusion during software AES. */
#include "tinycrypt/aes.h"
static struct {
    struct tc_aes_key_sched_struct aes;
    uint8_t key[16],ctr[16],mac[16],block[16],stream[16],plain[27],data[31];
    uint8_t n,llid,stage,offset,pass,encrypt;
} verify __attribute__((section(".pool.ble")));
static void verify_begin(const uint8_t key[16],const uint8_t iv[8],uint64_t counter,
                         int direction,uint8_t llid,const uint8_t *p,uint8_t n)
{
    ble_secure_zero(&verify,sizeof verify);
    if(n<5 || n>31 || counter>BLE_CCM_LAST) { verify.stage=255; return; }
    memcpy(verify.key,key,16); memcpy(verify.data,p,n); verify.n=(uint8_t)(n-4u); verify.llid=llid&3u;
    verify.ctr[0]=1;
    for(unsigned i=0;i<5;i++) verify.ctr[1+i]=(uint8_t)(counter>>(8u*i));
    verify.ctr[5]|=direction ? 0x80u : 0; memcpy(verify.ctr+6,iv,8);
}
static int verify_step(void)
{
    uint8_t *b=verify.block; unsigned off=verify.offset;
    switch(verify.stage) {
    case 0:
        tc_aes128_set_encrypt_key(&verify.aes,verify.key); ble_secure_zero(verify.key,16);
        b[0]=0x49; memcpy(b+1,verify.ctr+1,13); b[15]=verify.n; break;
    case 1:
        tc_aes_encrypt(verify.mac,b,&verify.aes);
        memset(b,0,16); b[1]=1; b[2]=verify.llid; break;
    case 2:
        for(unsigned i=0;i<16;i++) b[i]^=verify.mac[i];
        tc_aes_encrypt(verify.mac,b,&verify.aes); break;
    case 3:
        verify.ctr[15]=(uint8_t)(off/16u+1u);
        tc_aes_encrypt(verify.stream,verify.ctr,&verify.aes);
        memset(b,0,16);
        for(unsigned i=0;i<16 && off+i<verify.n;i++) {
            b[i]=verify.plain[off+i]=verify.encrypt ? verify.data[off+i] : verify.data[off+i]^verify.stream[i];
            if(verify.encrypt) verify.data[off+i]^=verify.stream[i];
        }
        break;
    case 4:
        for(unsigned i=0;i<16;i++) b[i]^=verify.mac[i];
        tc_aes_encrypt(verify.mac,b,&verify.aes);
        verify.offset=(uint8_t)(off+16u);
        if(verify.offset<verify.n) { verify.stage=3; return 0; }
        break;
    case 5: {
        uint8_t diff=0;
        verify.ctr[15]=0; tc_aes_encrypt(verify.stream,verify.ctr,&verify.aes);
        for(unsigned i=0;i<4;i++) {
            uint8_t tag=verify.mac[i]^verify.stream[i];
            if(verify.encrypt) verify.data[verify.n+i]=tag;
            else diff|=tag^verify.data[verify.n+i];
        }
        verify.pass=!diff; verify.stage=6;
        /* Consume/wipe in a separate item, after this AES block. */
        return 0;
    }
    default: return 1;
    }
    verify.stage++; return 0;
}
