/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mbedtls/fm1_crypto.h"
#include "tinycrypt/ecc.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "../third_party/nimble/nimble/host/src/ble_sm_priv.h"
#include "ble_s3_sim.h"
#include "ble_seed.h"
#include "ble_nimble110_vectors.inc"

static unsigned rng_calls;
static int test_rng(void *arg,unsigned char *out,size_t n)
{
    rng_calls++;
    if(!arg) return -1;
    memset(out,*(uint8_t *)arg,n); return 0;
}
static int old_rng(uint8_t *out,unsigned n) { memset(out,7,n); return 1; }
static void zeroed(const void *p,size_t n)
{
    const uint8_t *s=p;
    for(size_t i=0;i<n;i++) assert(!s[i]);
}
static void ecc_adapter(void)
{
    mbedtls_ecp_group g; mbedtls_ecp_point p; mbedtls_mpi d,shared;
    mbedtls_ecp_keypair key;
    uint8_t public_key[65]={4},private_le[32]={2},out[32],z=1;
    const uint8_t x2[32]={0x7c,0xf2,0x7b,0x18,0x8d,0x03,0x4f,0x7e,
        0x8a,0x52,0x38,0x03,0x04,0xb5,0x1a,0xc3,0xc0,0x89,0x69,0xe2,
        0x77,0xf2,0x1b,0x35,0xa6,0x0b,0x48,0xfc,0x47,0x66,0x99,0x78};
    mbedtls_ecp_group_init(&g); mbedtls_ecp_point_init(&p);
    mbedtls_mpi_init(&d); mbedtls_mpi_init(&shared); mbedtls_ecp_keypair_init(&key);
    assert(mbedtls_ecp_group_load(&g,99));
    assert(!mbedtls_ecp_group_load(&g,MBEDTLS_ECP_DP_SECP256R1));
    /* Valid generator point, scalar two, independently fixed affine x(2G).
     * Two different random projective Z values produce the same result. */
    uECC_vli_nativeToBytes(public_key+1,32,curve_secp256r1.G);
    uECC_vli_nativeToBytes(public_key+33,32,curve_secp256r1.G+8);
    assert(!mbedtls_ecp_point_read_binary(&g,&p,public_key,65));
    assert(!mbedtls_ecp_check_pubkey(&g,&p));
    assert(!mbedtls_mpi_read_binary_le(&d,private_le,32));
    uECC_set_rng(old_rng);
    for(z=1;z<=2;z++) {
        unsigned before=rng_calls;
        assert(!mbedtls_ecdh_compute_shared(&g,&shared,&p,&d,test_rng,&z));
        assert(rng_calls==before+1 && uECC_get_rng()==old_rng && !memcmp(shared.be,x2,32));
        assert(!mbedtls_mpi_write_binary_le(&shared,out,32));
        for(unsigned i=0;i<32;i++) assert(out[i]==x2[31u-i]);
    }
    assert(mbedtls_ecdh_compute_shared(&g,&shared,&p,&d,test_rng,NULL));
    zeroed(&shared,sizeof shared); assert(uECC_get_rng()==old_rng);
    assert(mbedtls_ecdh_compute_shared(&g,&shared,&p,&d,NULL,NULL));
    memset(private_le,0,32); assert(!mbedtls_mpi_read_binary_le(&d,private_le,32));
    assert(mbedtls_ecdh_compute_shared(&g,&shared,&p,&d,test_rng,&z));
    uECC_vli_nativeToBytes(d.be,32,curve_secp256r1.n);
    assert(mbedtls_ecdh_compute_shared(&g,&shared,&p,&d,test_rng,&z));
    assert(mbedtls_mpi_read_binary_le(&d,private_le,33));
    assert(mbedtls_mpi_write_binary_le(&d,out,31));
    public_key[0]=2; assert(mbedtls_ecp_point_read_binary(&g,&p,public_key,65));
    public_key[0]=4; assert(mbedtls_ecp_point_read_binary(&g,&p,public_key,64));
    public_key[64]^=1; assert(!mbedtls_ecp_point_read_binary(&g,&p,public_key,65));
    assert(mbedtls_ecp_check_pubkey(&g,&p));
    assert(mbedtls_ecdh_compute_shared(&g,&shared,&p,&d,test_rng,&z));
    memset(public_key+1,0,64); assert(!mbedtls_ecp_point_read_binary(&g,&p,public_key,65));
    assert(mbedtls_ecp_check_pubkey(&g,&p));
    uECC_vli_nativeToBytes(public_key+1,32,curve_secp256r1.p);
    assert(!mbedtls_ecp_point_read_binary(&g,&p,public_key,65)); assert(mbedtls_ecp_check_pubkey(&g,&p));
    assert(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,&key,test_rng,NULL));
    zeroed(&key,sizeof key); assert(uECC_get_rng()==old_rng);
    assert(mbedtls_ecp_gen_key(99,&key,test_rng,&z));
    assert(!mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,&key,test_rng,&z));
    assert(uECC_get_rng()==old_rng);
    assert(!mbedtls_ecp_export(&key,&g,&d,&p));
    size_t n=0;
    assert(!mbedtls_ecp_point_write_binary(&g,&p,MBEDTLS_ECP_PF_UNCOMPRESSED,&n,public_key,65) && n==65);
    assert(mbedtls_ecp_point_write_binary(&g,&p,1,&n,public_key,65));
    assert(mbedtls_ecp_point_write_binary(&g,&p,0,&n,public_key,64));
    mbedtls_ecp_keypair_free(&key); zeroed(&key,sizeof key);
    mbedtls_mpi_free(&d); mbedtls_mpi_free(&shared); zeroed(&d,sizeof d); zeroed(&shared,sizeof shared);
    mbedtls_ecp_point_free(&p); zeroed(&p,sizeof p); mbedtls_ecp_group_free(&g);
    uECC_set_rng(NULL);
}
int main(void)
{
    mbedtls_aes_context aes;
    uint8_t out[16],key[16]={0},in[16]={0};
    const uint8_t expected[16]={0x66,0xe9,0x4b,0xd4,0xef,0x8a,0x2c,0x3b,0x88,0x4c,0xfa,0x59,0xca,0x34,0x2b,0x2e};
    mbedtls_aes_init(&aes); assert(mbedtls_aes_crypt_ecb(&aes,1,in,out));
    assert(mbedtls_aes_setkey_enc(&aes,key,256));
    assert(!mbedtls_aes_setkey_enc(&aes,key,128));
    assert(!mbedtls_aes_crypt_ecb(&aes,1,in,out) && !memcmp(out,expected,16));
    assert(mbedtls_aes_crypt_ecb(&aes,0,in,out));
    mbedtls_aes_free(&aes); zeroed(&aes,sizeof aes);
    assert(!mbedtls_cipher_info_from_type(99));
    assert(mbedtls_cipher_cmac(NULL,key,128,in,16,out));
    const mbedtls_cipher_info_t *info=mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    assert(mbedtls_cipher_cmac(info,key,256,in,16,out));
    ble_sm_test_case_f4(); ble_sm_test_case_f5(); ble_sm_test_case_f6();
    ecc_adapter();
    puts("NimBLE 1.10 crypto: AES/f4/f5/f6 vectors, P-256 generator/2G, endian, blinded ECDH, invalid points/scalars, RNG failure/restoration, context erasure: PASS");
    return 0;
}
