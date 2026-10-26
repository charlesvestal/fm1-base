/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#include <string.h>
#include "ble_s2_sim.h"
#include "../open_ble/ble_timing.h"
#if FELUCCA_BLE_STAGE >= 3
#include "../open_ble/ble_security.h"
uint8_t sim_air[2][31],sim_air_len[2];
#endif
uint32_t sim_ticks,sim_starts,sim_stops,sim_windows,sim_applies;
ble_conn_t sim_applied;
uint8_t sim_apply_kind;
uint16_t sim_apply_event,sim_registered_instant;
uint32_t sim_apply_ticks;
int sim_executor_enabled;
void (*sim_lock_hook)(void);
int sim_stop_result=1,sim_slot[2]={-1,-1},sim_latency_enabled;
int sim_start_steps,sim_start_failure;
ble_adv_t sim_advertising;
static int stopping;
#ifndef BLE_MIC_RADIO
static int refilling;
#endif
static uint32_t clock_last,clock_ms,clock_fraction;
uint32_t ble_port_ticks(void) { return sim_ticks; }
uint32_t ble_port_ms(void)
{
    uint32_t delta=sim_ticks-clock_last;
    clock_last=sim_ticks; clock_ms+=delta/24000u;
    clock_fraction+=delta%24000u;
    if(clock_fraction>=24000u) { clock_fraction-=24000u; clock_ms++; }
    return clock_ms;
}
int ble_port_start(const ble_adv_t *adv)
{
    if(sim_start_failure) return -1;
    if(sim_start_steps) { sim_start_steps--; return 1; }
    sim_advertising=*adv; sim_starts++; return 0;
}
void ble_port_update(const ble_adv_t *adv) { sim_advertising=*adv; }
void ble_port_stop_begin(void) { sim_stops++; stopping=1; }
int ble_port_stop_poll(void)
{
    if(stopping && sim_stop_result>0) { sim_slot[0]=sim_slot[1]=-1; stopping=0; }
    return sim_stop_result;
}
int ble_port_failed(void) { return sim_stop_result<0; }
void ble_port_reset_stats(void) { ble_time_reset(); }
uint32_t ble_port_lock(void)
{
    if(sim_lock_hook) { void (*hook)(void)=sim_lock_hook; sim_lock_hook=0; hook(); }
    return 0;
}
void ble_port_unlock(uint32_t key) { (void)key; }
#ifdef BLE_MIC_RADIO
#include "ble_mic_radio.inc"
#else
#if FM1B_BLE_RELIABILITY
int ble_port_diag_rx_read(unsigned slot,uint16_t *header,uint16_t *status,uint8_t out[31])
{ (void)slot; (void)header; (void)status; (void)out; return 0; }
void ble_port_diag_rx_release(unsigned slot) { (void)slot; }
int ble_port_diag_radio_idle(void) { return 1; }
#endif
static void refill(unsigned limit)
{
    unsigned i;
    if(refilling) return;
    refilling=1;
    for(i=0;i<2 && limit;i++) if(sim_slot[i]<0) {
        sim_slot[i]=ble_ll_tx_pick();
        if(sim_slot[i]>=0) limit--;
#if FELUCCA_BLE_STAGE >= 3
        if(sim_slot[i]>=0 && !ble_ll_tx_prepare((unsigned)sim_slot[i],sim_air[i],&sim_air_len[i])) sim_slot[i]=-1;
#endif
    }
    refilling=0;
}
void ble_port_refill(void) { refill(2); }
void ble_port_refill_one(void) { refill(1); }
void ble_port_rx_recover(void) { }
#endif
void ble_port_window(uint32_t us) { sim_windows=us; }
void ble_port_instant(uint8_t kind,uint16_t instant,const ble_conn_t *c) { (void)kind; (void)c; sim_registered_instant=instant; }
void ble_port_apply(uint8_t kind,const ble_conn_t *c,uint16_t w,uint32_t win)
{
    (void)w; sim_applied=*c; sim_apply_kind=kind; sim_apply_event=ble_link.closed_n;
    sim_apply_ticks=sim_ticks; sim_windows=win; sim_applies++;
}
void ble_port_latency(uint16_t latency,int enabled) { sim_latency_enabled=enabled && latency; }
void ble_port_priority(unsigned priority) { (void)priority; }
void ble_port_pend(void) { }
void ble_port_host_idle(void) { sim_advance_us(100); ble_worker(); }
void sim_advance_us(uint32_t us) { sim_ticks+=us*24u; ble_ll_worker(); if(sim_executor_enabled) ble_worker(); }
void sim_ack(unsigned slot)
{
#ifdef BLE_MIC_RADIO
    sim_radio_ack(slot);
#else
    int id=sim_slot[slot]; sim_slot[slot]=-1;
    if(id>=0) {
#if FELUCCA_BLE_STAGE >= 3
        uint8_t marker=ble_ll_tx_boundary((unsigned)id);
        if(marker) (void)ble_ll_rx_marker(marker);
#endif
        ble_ll_tx_ack((unsigned)id);
    }
    ble_port_refill();
#endif
}
void sim_reset(void)
{
    sim_ticks=24000; clock_last=sim_ticks; clock_ms=1; clock_fraction=0; sim_starts=sim_stops=sim_windows=sim_applies=0;
    sim_stop_result=1; sim_start_steps=sim_start_failure=0; stopping=0;
    sim_latency_enabled=0; sim_slot[0]=sim_slot[1]=-1; sim_lock_hook=0; ble_ll_init();
#ifdef BLE_MIC_RADIO
    sim_radio_reset();
#else
    refilling=0;
#endif
    memset(&sim_applied,0,sizeof sim_applied); sim_apply_kind=0;
    sim_apply_event=sim_registered_instant=0; sim_apply_ticks=0;
}
ble_conn_t sim_connection(void)
{
    ble_conn_t c;
    memset(&c,0,sizeof c); c.interval=24; c.timeout=500; c.win=1; c.hop=5;
    c.aa=0x12345678; c.crc=0x123456; memset(c.map,255,4); c.map[4]=31;
    c.peer[0]=1; return c;
}
