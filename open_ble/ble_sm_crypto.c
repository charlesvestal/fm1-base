/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#include <string.h>
#include "syscfg/syscfg.h"
#include "mbedtls/fm1_crypto.h"
#include "tinycrypt/cmac_mode.h"
#include "tinycrypt/constants.h"
#include "tinycrypt/ecc.h"
#include "tinycrypt/ecc_dh.h"
#include "ble_crypto.h"

void mbedtls_aes_init(mbedtls_aes_context *ctx) { memset(ctx,0,sizeof *ctx); }
void mbedtls_aes_free(mbedtls_aes_context *ctx) { ble_secure_zero(ctx,sizeof *ctx); }
int mbedtls_aes_setkey_enc(mbedtls_aes_context *ctx,const unsigned char *key,unsigned bits)
{
    ctx->ready=0;
    if(bits!=128 || !key) return -1;
    ctx->ready=tc_aes128_set_encrypt_key(&ctx->schedule,key)==TC_CRYPTO_SUCCESS;
    return ctx->ready ? 0 : -1;
}
int mbedtls_aes_crypt_ecb(mbedtls_aes_context *ctx,int mode,const unsigned char *in,unsigned char *out)
{
    if(!ctx->ready || mode!=MBEDTLS_AES_ENCRYPT || !in || !out) return -1;
    return tc_aes_encrypt(out,in,&ctx->schedule)==TC_CRYPTO_SUCCESS ? 0 : -1;
}
const mbedtls_cipher_info_t *mbedtls_cipher_info_from_type(int type)
{
    static const mbedtls_cipher_info_t aes={MBEDTLS_CIPHER_AES_128_ECB};
    return type==aes.type ? &aes : NULL;
}
int mbedtls_cipher_cmac(const mbedtls_cipher_info_t *info,const unsigned char *key,size_t bits,
                        const unsigned char *in,size_t len,unsigned char *out)
{
    struct tc_aes_key_sched_struct schedule;
    struct tc_cmac_struct state;
    int ok;
    if(!info || info->type!=MBEDTLS_CIPHER_AES_128_ECB || bits!=128 || !key || !out || (!in && len)) return -1;
    ok=tc_cmac_setup(&state,key,&schedule)==TC_CRYPTO_SUCCESS &&
       tc_cmac_update(&state,in,len)==TC_CRYPTO_SUCCESS &&
       tc_cmac_final(out,&state)==TC_CRYPTO_SUCCESS;
    ble_secure_zero(&schedule,sizeof schedule); ble_secure_zero(&state,sizeof state);
    return ok ? 0 : -1;
}

void mbedtls_mpi_init(mbedtls_mpi *v) { memset(v,0,sizeof *v); }
void mbedtls_mpi_free(mbedtls_mpi *v) { ble_secure_zero(v,sizeof *v); }
int mbedtls_mpi_read_binary_le(mbedtls_mpi *v,const unsigned char *in,size_t n)
{
    if(n!=32 || !in) return -1;
    for(unsigned i=0;i<32;i++) v->be[i]=in[31u-i];
    return 0;
}
int mbedtls_mpi_write_binary_le(const mbedtls_mpi *v,unsigned char *out,size_t n)
{
    if(n!=32 || !out) return -1;
    for(unsigned i=0;i<32;i++) out[i]=v->be[31u-i];
    return 0;
}
void mbedtls_ecp_group_init(mbedtls_ecp_group *g) { g->id=0; }
void mbedtls_ecp_group_free(mbedtls_ecp_group *g) { g->id=0; }
int mbedtls_ecp_group_load(mbedtls_ecp_group *g,int id)
{
    g->id=id==MBEDTLS_ECP_DP_SECP256R1 ? id : 0;
    return g->id ? 0 : -1;
}
void mbedtls_ecp_point_init(mbedtls_ecp_point *p) { memset(p,0,sizeof *p); }
void mbedtls_ecp_point_free(mbedtls_ecp_point *p) { ble_secure_zero(p,sizeof *p); }
int mbedtls_ecp_point_read_binary(const mbedtls_ecp_group *g,mbedtls_ecp_point *p,const unsigned char *in,size_t n)
{
    p->present=0;
    if(g->id!=MBEDTLS_ECP_DP_SECP256R1 || n!=65 || !in || in[0]!=4) return -1;
    memcpy(p->be,in+1,64); p->present=1; return 0;
}
int mbedtls_ecp_check_pubkey(const mbedtls_ecp_group *g,const mbedtls_ecp_point *p)
{
    uECC_word_t point[16]; int rc;
    if(g->id!=MBEDTLS_ECP_DP_SECP256R1 || !p->present) return -1;
    uECC_vli_bytesToNative(point,p->be,32); uECC_vli_bytesToNative(point+8,p->be+32,32);
    /* Check curve membership, coordinates < p, and infinity. Unlike the old
     * uECC_valid_public_key API, the valid generator point is accepted. */
    rc=uECC_valid_point(point,&curve_secp256r1);
    ble_secure_zero(point,sizeof point); return rc ? -1 : 0;
}
int mbedtls_ecp_point_write_binary(const mbedtls_ecp_group *g,const mbedtls_ecp_point *p,
                                  int format,size_t *len,unsigned char *out,size_t n)
{
    if(format!=MBEDTLS_ECP_PF_UNCOMPRESSED || n<65 || !len || !out || mbedtls_ecp_check_pubkey(g,p)) return -1;
    out[0]=4; memcpy(out+1,p->be,64); *len=65; return 0;
}
void mbedtls_ecp_keypair_init(mbedtls_ecp_keypair *k) { memset(k,0,sizeof *k); }
void mbedtls_ecp_keypair_free(mbedtls_ecp_keypair *k) { ble_secure_zero(k,sizeof *k); }

/* TinyCrypt's RNG is global; scope it to this synchronous main-context call,
 * restore any previous user, and reject re-entry. Never install a fallback
 * RNG. NimBLE supplies ble_sm_alg_rng -> LE Rand -> boot-seeded entropy. */
static fm1_crypto_rng active_rng;
static void *active_arg;
static uECC_RNG_Function saved_rng;
static int rng_bytes(uint8_t *out,unsigned n) { return active_rng(active_arg,out,n)==0; }
static int rng_begin(fm1_crypto_rng rng,void *arg)
{
    if(!rng || active_rng) return -1;
    active_rng=rng; active_arg=arg; saved_rng=uECC_get_rng(); uECC_set_rng(rng_bytes); return 0;
}
static void rng_end(void)
{
    uECC_set_rng(saved_rng); saved_rng=NULL; active_rng=NULL; active_arg=NULL;
}
int mbedtls_ecp_gen_key(int id,mbedtls_ecp_keypair *k,fm1_crypto_rng rng,void *arg)
{
    int ok;
    mbedtls_ecp_keypair_free(k);
    if(id!=MBEDTLS_ECP_DP_SECP256R1 || rng_begin(rng,arg)) return -1;
    ok=uECC_make_key(k->q.be,k->d.be,&curve_secp256r1);
    rng_end();
    if(!ok) { mbedtls_ecp_keypair_free(k); return -1; }
    k->group.id=id; k->q.present=1; return 0;
}
int mbedtls_ecp_export(const mbedtls_ecp_keypair *k,mbedtls_ecp_group *g,mbedtls_mpi *d,mbedtls_ecp_point *p)
{
    if(mbedtls_ecp_check_pubkey(&k->group,&k->q)) return -1;
    *g=k->group; *d=k->d; *p=k->q; return 0;
}
int mbedtls_ecdh_compute_shared(const mbedtls_ecp_group *g,mbedtls_mpi *shared,
                                const mbedtls_ecp_point *q,const mbedtls_mpi *d,fm1_crypto_rng rng,void *arg)
{
    uECC_word_t point[16],scalar[8],k0[8],k1[8],z[8];
    uECC_word_t carry;
    int rc=-1;
    mbedtls_mpi_free(shared);
    if(mbedtls_ecp_check_pubkey(g,q)) return -1;
    uECC_vli_bytesToNative(scalar,d->be,32);
    if(uECC_vli_isZero(scalar,8) || uECC_vli_cmp(curve_secp256r1.n,scalar,8)!=1) goto done;
    if(rng_begin(rng,arg)) goto done;
    /* The retained ecc_dh.c has a separate, unset RNG pointer. Use the
     * existing ECC primitives with an explicit random projective Z so the
     * new per-call RNG contract also blinds shared-secret multiplication. */
    if(!uECC_generate_random_int(z,curve_secp256r1.p,8)) { rng_end(); goto done; }
    rng_end();
    uECC_vli_bytesToNative(point,q->be,32); uECC_vli_bytesToNative(point+8,q->be+32,32);
    carry=regularize_k(scalar,k0,k1,&curve_secp256r1);
    EccPoint_mult(point,point,carry ? k0 : k1,z,257,&curve_secp256r1);
    if(!EccPoint_isZero(point,&curve_secp256r1)) {
        uECC_vli_nativeToBytes(shared->be,32,point); rc=0;
    }
done:
    ble_secure_zero(point,sizeof point); ble_secure_zero(scalar,sizeof scalar);
    ble_secure_zero(k0,sizeof k0); ble_secure_zero(k1,sizeof k1); ble_secure_zero(z,sizeof z);
    return rc;
}
