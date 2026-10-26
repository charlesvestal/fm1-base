/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FM1_NIMBLE_CRYPTO_API_H
#define FM1_NIMBLE_CRYPTO_API_H
/* Private source adapter for the calls in NimBLE 1.10 ble_sm_alg.c only.
 * This is NOT Mbed TLS, its ABI, or a general implementation of its API.
 * AES-128 encryption, one-shot CMAC and uncompressed P-256 are backed by
 * the retained TinyCrypt pin. Unsupported inputs fail. No heap is used. */
#include <stddef.h>
#include <stdint.h>
#include "tinycrypt/aes.h"

#define MBEDTLS_AES_ENCRYPT 1
#define MBEDTLS_CIPHER_AES_128_ECB 1
#define MBEDTLS_ECP_DP_SECP256R1 1
#define MBEDTLS_ECP_PF_UNCOMPRESSED 0
typedef int (*fm1_crypto_rng)(void *, unsigned char *, size_t);
typedef struct mbedtls_aes_context {
    struct tc_aes_key_sched_struct schedule;
    int ready;
} mbedtls_aes_context;
typedef struct { int type; } mbedtls_cipher_info_t;
typedef struct { uint8_t be[32]; } mbedtls_mpi;
typedef struct { int id; } mbedtls_ecp_group;
typedef struct { uint8_t be[64]; int present; } mbedtls_ecp_point;
typedef struct {
    mbedtls_ecp_group group;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
} mbedtls_ecp_keypair;

void mbedtls_aes_init(mbedtls_aes_context *);
void mbedtls_aes_free(mbedtls_aes_context *);
int mbedtls_aes_setkey_enc(mbedtls_aes_context *, const unsigned char *, unsigned);
int mbedtls_aes_crypt_ecb(mbedtls_aes_context *, int, const unsigned char *, unsigned char *);
const mbedtls_cipher_info_t *mbedtls_cipher_info_from_type(int);
int mbedtls_cipher_cmac(const mbedtls_cipher_info_t *, const unsigned char *, size_t,
                        const unsigned char *, size_t, unsigned char *);
void mbedtls_mpi_init(mbedtls_mpi *);
void mbedtls_mpi_free(mbedtls_mpi *);
int mbedtls_mpi_read_binary_le(mbedtls_mpi *, const unsigned char *, size_t);
int mbedtls_mpi_write_binary_le(const mbedtls_mpi *, unsigned char *, size_t);
void mbedtls_ecp_group_init(mbedtls_ecp_group *);
void mbedtls_ecp_group_free(mbedtls_ecp_group *);
int mbedtls_ecp_group_load(mbedtls_ecp_group *, int);
void mbedtls_ecp_point_init(mbedtls_ecp_point *);
void mbedtls_ecp_point_free(mbedtls_ecp_point *);
int mbedtls_ecp_point_read_binary(const mbedtls_ecp_group *, mbedtls_ecp_point *, const unsigned char *, size_t);
int mbedtls_ecp_check_pubkey(const mbedtls_ecp_group *, const mbedtls_ecp_point *);
int mbedtls_ecp_point_write_binary(const mbedtls_ecp_group *, const mbedtls_ecp_point *,
                                  int, size_t *, unsigned char *, size_t);
void mbedtls_ecp_keypair_init(mbedtls_ecp_keypair *);
void mbedtls_ecp_keypair_free(mbedtls_ecp_keypair *);
int mbedtls_ecp_gen_key(int, mbedtls_ecp_keypair *, fm1_crypto_rng, void *);
int mbedtls_ecp_export(const mbedtls_ecp_keypair *, mbedtls_ecp_group *, mbedtls_mpi *, mbedtls_ecp_point *);
int mbedtls_ecdh_compute_shared(const mbedtls_ecp_group *, mbedtls_mpi *,
                                const mbedtls_ecp_point *, const mbedtls_mpi *, fm1_crypto_rng, void *);
#endif
