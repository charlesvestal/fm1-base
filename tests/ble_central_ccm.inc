/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Test-only CCM oracle using public TinyCrypt AES directly. It does not call
 * the controller CCM routine, AES port, security state, or counter helpers. */
#include "tinycrypt/aes.h"
static int central_ccm(uint64_t count,int direction,uint8_t llid,uint8_t *p,unsigned length,int decrypt)
{
    struct tc_aes_key_sched_struct aes;
    uint8_t nonce[13],b[16]={0},mac[16]={0},a[16]={1},stream[16],text[27];
    if(!length || length>27 || count>>39) return 0;
    assert(tc_aes128_set_encrypt_key(&aes,sk));
    for(unsigned i=0;i<5;i++) nonce[i]=(uint8_t)(count>>(8*i));
    nonce[4]|=(uint8_t)(direction ? 0x80u : 0); memcpy(nonce+5,iv,8);
    b[0]=0x49; memcpy(b+1,nonce,13); b[15]=(uint8_t)length;
    assert(tc_aes_encrypt(mac,b,&aes));
    memset(b,0,16); b[1]=1; b[2]=llid&3u;
    for(unsigned i=0;i<16;i++) b[i]^=mac[i];
    assert(tc_aes_encrypt(mac,b,&aes));
    memcpy(a+1,nonce,13);
    for(unsigned off=0;off<length;off+=16) {
        a[15]=(uint8_t)(1+off/16); assert(tc_aes_encrypt(stream,a,&aes));
        memset(b,0,16);
        for(unsigned j=0;j<16 && off+j<length;j++) {
            text[off+j]=decrypt ? p[off+j]^stream[j] : p[off+j]; b[j]=text[off+j];
            if(!decrypt) p[off+j]^=stream[j];
        }
        for(unsigned j=0;j<16;j++) b[j]^=mac[j];
        assert(tc_aes_encrypt(mac,b,&aes));
    }
    a[15]=0; assert(tc_aes_encrypt(stream,a,&aes));
    uint8_t diff=0;
    for(unsigned i=0;i<4;i++) {
        uint8_t tag=mac[i]^stream[i];
        if(decrypt) diff|=tag^p[length+i]; else p[length+i]=tag;
    }
    if(decrypt && !diff) memcpy(p,text,length);
    return !diff;
}
static void central_ccm_test(void)
{
    begin();
    for(unsigned direction=0;direction<2;direction++) for(unsigned length=1;length<=27;length++) {
        uint8_t original[31],air[31]; uint64_t count=((uint64_t)1<<38)+length;
        for(unsigned j=0;j<length;j++) original[j]=(uint8_t)(length+j);
        memcpy(air,original,length); assert(central_ccm(count,(int)direction,3,air,length,0));
        assert(ble_ccm(sk,iv,count,(int)direction,3,air,(uint8_t)length,1));
        assert(!memcmp(air,original,length));
        memcpy(air,original,length); assert(ble_ccm(sk,iv,count,(int)direction,3,air,(uint8_t)length,0));
        uint8_t damaged[31]; memcpy(damaged,air,length+4); damaged[length+2]^=1;
        assert(!central_ccm(count,(int)direction,3,damaged,length,1));
        assert(central_ccm(count,(int)direction,3,air,length,1)); assert(!memcmp(air,original,length));
    }
    puts("independent central CCM: both directions, lengths 1..27, high counters, corrupted tags rejected: PASS");
}
