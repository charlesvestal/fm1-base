/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* r4 02 §8.4: standard-order AES block entry, private to BLE.
 * Key/data FIFO words go MSW first; result registers are reversed MSW/LSW.
 * The entire transaction has a 26-us deadline and holds IRQ exclusion. */
#pragma once
#include "../ble_crypto.h"
static uint32_t ble_aes_irq_key,ble_aes_started;
#if FM1B_BLE_RELIABILITY
static ble_aes_state_t aes_state;
void ble_port_aes_state(ble_aes_state_t *out)
{
    uint32_t k=fm1_ble_irq_save(); *out=aes_state; out->con=BR_R(0x41200u);
    fm1_ble_irq_restore(k);
}
#endif
static void fm1_ble_r64_snapshot(uint32_t out[2])
{
    out[0]=BR_R(0x13b00u); out[1]=BR_R(0x13b04u);
}
static int ble_aes_wait(uint32_t bit)
{
    for(;;) {
        if((uint32_t)(fm1_ticks()-ble_aes_started)>=26u*24u) {
#if FM1B_BLE_RELIABILITY
            aes_state.timeouts++;
#endif
            br_error(11); return 0; }
        if(BR_R(0x41200u)&bit) break;
    }
    return 1;
}
static uint32_t ble_aes_word(const uint8_t *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
int ble_port_aes_begin(const uint8_t key[16])
{
    unsigned i;
    ble_aes_irq_key=fm1_ble_irq_save(); ble_aes_started=fm1_ticks();
    #if FM1B_BLE_RELIABILITY
    aes_state.busy=1; aes_state.epoch=ble_link.epoch; aes_state.operations++;
#endif
    for(i=0;i<4;i++) BR_R(0x41208u)=ble_aes_word(key+4u*i);
    BR_R(0x41200u)=(BR_R(0x41200u)&~64u)|16u;
    if(!ble_aes_wait(32u)) return 0;
    BR_R(0x41200u)|=1u<<13; return 1;
}
int ble_port_aes_block(const uint8_t in[16],uint8_t out[16])
{
    unsigned i;
    #if FM1B_BLE_RELIABILITY
    aes_state.blocks++;
#endif
    for(i=0;i<4;i++) BR_R(0x41204u)=ble_aes_word(in+4u*i);
    BR_R(0x41200u)|=1u;
    if(!ble_aes_wait(4u)) return 0;
    BR_R(0x41200u)|=1u<<12;
    for(i=0;i<4;i++) {
        uint32_t v=BR_R(0x4120cu+4u*(3u-i));
        out[4*i]=(uint8_t)(v>>24); out[4*i+1]=(uint8_t)(v>>16);
        out[4*i+2]=(uint8_t)(v>>8); out[4*i+3]=(uint8_t)v;
    }
    return 1;
}
void ble_port_aes_end(void) {
#if FM1B_BLE_RELIABILITY
    aes_state.busy=0; aes_state.last_epoch=aes_state.epoch;
#endif
    fm1_ble_irq_restore(ble_aes_irq_key);
}
