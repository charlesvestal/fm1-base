/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
#include <assert.h>
#include <stdio.h>
#include "../open_ble/ble_health.h"
int main(void)
{
    uint32_t t=0xffff0000u;
    ble_health_init(t,1234);
    ble_health_begin(BLE_GAP_BOOT);
    ble_health_sample(t,BLE_HEALTH_STARTING,0);
    ble_health_end(BLE_GAP_BOOT);
    t+=7500u*24u; ble_health_sample(t,BLE_HEALTH_ADVERTISING,0);
    assert(!ble_health.over && ble_health.maximum.ms==1241);
    assert(ble_health.maximum.reasons==BLE_GAP_BOOT);
    ble_health_begin(BLE_GAP_FLASH); ble_health_begin(BLE_GAP_FLASH);
    ble_health_end(BLE_GAP_FLASH); assert(ble_health.active&BLE_GAP_FLASH);
    ble_health_end(BLE_GAP_FLASH); /* IRQ may run after the scope has ended. */
    t+=17804u*24u; ble_health_sample(t,BLE_HEALTH_OFF,7);
    assert(ble_health.over==1 && ble_health.last[0].ms==1259);
    assert(ble_health.maximum.ticks==17804u*24u && ble_health.maximum.epoch==7);
    assert(ble_health.last[0].reasons==BLE_GAP_FLASH);
    for(unsigned i=0;i<5;i++) {
        t+=8000u*24u; ble_health_sample(t,BLE_HEALTH_LINK,10+i);
    }
    assert(ble_health.count==4 && ble_health.over==6);
    assert(ble_health.last[(ble_health.next+3u)%4u].epoch==14);
    assert(ble_health.maximum.ticks==17804u*24u); /* maximum survives ring overwrite */
    for(unsigned i=0;i<4;i++) assert(!ble_health.last[i].reasons && ble_health.last[i].state==BLE_HEALTH_LINK);
    for(unsigned i=0;i<6;i++) {
        ble_health_adv_begin(t);
        ble_health_connect(0); ble_health_connect(0); ble_health_connect(1);
        assert(ble_health.adv.session==i+1 && ble_health.adv.seen==2 && ble_health.adv.accepted==1);
    }
#if FM1B_BLE_DIAG
    assert(ble_health.sessions[1].session==6);
#endif
    puts("gap attribution: tick wrap, strict threshold, time/state/reasons/epoch, last four/max, session reset: PASS");
}
