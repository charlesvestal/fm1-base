/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
#include "ble_health.h"
#include "ble_memory.h"
ble_health_t ble_health;
void ble_health_init(uint32_t ticks,uint32_t ms)
{
    memset(&ble_health,0,sizeof ble_health);
    ble_health.clock_ticks=ticks; ble_health.clock_ms=ms;
    ble_health.initialized=1;
}
uint32_t ble_health_ms(uint32_t ticks)
{
    uint32_t dt=ticks-ble_health.clock_ticks;
    ble_health.clock_ticks=ticks;
    ble_health.clock_ms+=dt/24000u; ble_health.fraction+=dt%24000u;
    if(ble_health.fraction>=24000u) { ble_health.fraction-=24000u; ble_health.clock_ms++; }
    return ble_health.clock_ms;
}
void ble_health_sample(uint32_t ticks,uint8_t state,uint32_t epoch)
{
    ble_gap_event_t e={ble_health_ms(ticks),ticks-ble_health.last_ticks,epoch,
        ble_health.state,state,(uint8_t)(ble_health.active|ble_health.since)};
    if(ble_health.calls) {
        /* Any link state at either endpoint makes this a live-link gap. */
        if(state==BLE_HEALTH_LINK) e.state=BLE_HEALTH_LINK;
        if(e.ticks>ble_health.maximum.ticks) ble_health.maximum=e;
        if(e.ticks>7500u*24u) {
            ble_health.last[ble_health.next]=e;
            ble_health.next=(uint8_t)((ble_health.next+1u)%4u);
            if(ble_health.count<4) ble_health.count++;
            ble_health.over++;
        }
    }
    ble_health.calls++; ble_health.last_ticks=ticks; ble_health.state=state;
    ble_health.since=ble_health.active;
}
void ble_health_begin(uint8_t reason)
{
    for(unsigned i=0;i<4;i++) if(reason&(1u<<i)) ble_health.depth[i]++;
    ble_health.active|=reason; ble_health.since|=reason;
}
void ble_health_end(uint8_t reason)
{
    for(unsigned i=0;i<4;i++) if(reason&(1u<<i)) {
        if(ble_health.depth[i]) ble_health.depth[i]--;
        if(!ble_health.depth[i]) ble_health.active&=(uint8_t)~(1u<<i);
    }
    ble_health.since|=reason;
}
void ble_health_adv_begin(uint32_t ticks)
{
    ble_health.adv.session++; ble_health.adv.ms=ble_health_ms(ticks);
    ble_health.adv.seen=ble_health.adv.accepted=0;
#if FM1B_BLE_DIAG
    ble_health.sessions[(ble_health.adv.session-1u)%4u]=ble_health.adv;
#endif
}
void ble_health_connect(int accepted)
{
    if(accepted) ble_health.adv.accepted++; else ble_health.adv.seen++;
#if FM1B_BLE_DIAG
    ble_health.sessions[(ble_health.adv.session-1u)%4u]=ble_health.adv;
#endif
}
