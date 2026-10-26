/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FELUCCA_BLE_CRYPTO_H
#define FELUCCA_BLE_CRYPTO_H
#include <stdint.h>
/* Standard AES octet order. begin/end hold the hardware exclusion once for
 * the complete CCM operation. No other user can replace its expanded key. */
int ble_port_aes_begin(const uint8_t key[16]);
int ble_port_aes_block(const uint8_t in[16],uint8_t out[16]);
void ble_port_aes_end(void);
int ble_aes_le(const uint8_t key[16],const uint8_t in[16],uint8_t out[16]);
int ble_ccm(const uint8_t key[16],const uint8_t iv[8],uint64_t counter,
    int central,uint8_t llid,uint8_t *data,uint8_t n,int decrypt);
int ble_crypto_selftest(void);
/* IRK, prand and hash in HCI (least significant octet first) order. */
int ble_ah(const uint8_t irk[16],const uint8_t prand[3],uint8_t hash[3]);
int ble_ah_selftest(void);
void ble_secure_zero(void *p,unsigned n);
#endif
