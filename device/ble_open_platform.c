/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wall"
#pragma clang diagnostic error "-Wextra"
/* fm1-base adapter over felucca-bsp. Register access stays in open_ble/hal.
 * BSP wrappers account nested baseband/audio/TIMER5 preemption once. */
#include "../open_ble/ble_s1.h"
#include "../open_ble/ble_health.h"
static ble_cal_t ble_cal;
static uint8_t ble_random_config[6],ble_started,ble_store_valid;
#include "../open_ble/ble_host.h"
#if FELUCCA_BLE_STAGE >= 3
#include "../open_ble/ble_seed.h"
#include "../open_ble/ble_bonds.h"
#endif
void ble_port_pend(void) { fm1_ble_worker_pend(); }
uint32_t ble_port_lock(void) { return fm1_ble_irq_save(); }
void ble_port_unlock(uint32_t key) { fm1_ble_irq_restore(key); }
void ble_port_refill(void) { bb_refill(); }
void ble_port_refill_one(void) { bb_refill_limit(1); }
void ble_port_rx_recover(void) { if(bb.state==4) bb_connected_rx(fm1_ticks(),1); }
void ble_port_window(uint32_t us) { bb_window(us); }
void ble_port_priority(unsigned p) { bb_priority(p); }
void ble_port_latency(uint16_t latency,int enabled) { bb_latency(latency,enabled); }
void ble_port_instant(uint8_t kind,uint16_t instant,const ble_conn_t *c)
{
    (void)kind; (void)c; bb_anchor(0,5,instant); bb_priority(30);
}
void ble_port_apply(uint8_t kind,const ble_conn_t *c,uint16_t widening,uint32_t window) { bb_apply(kind,c,widening,window); }
void ble_port_host_idle(void) { fm1_wdt_feed(); }
void fm1_ble_worker_irq(void)
{
    static uint32_t chain_start;
    uint32_t key=fm1_ble_irq_save(),start=fm1_ticks();
    uint8_t state=ble_link.state!=BLE_LINK_IDLE && ble_link.state!=BLE_LINK_STOPPED ? BLE_HEALTH_LINK :
        bb.state==1 ? BLE_HEALTH_ADVERTISING : bb.start_phase ? BLE_HEALTH_STARTING : ble_health.phase;
    ble_health_sample(start,state,ble_link.epoch);
    fm1_ble_worker_timer_ack(); ble_llx_running=1;
    if(ble_llx_pending) {
        ble_time_dispatch(ble_llx_request_ticks,start);
    }
    ble_llx_pending=0;
    ble_llx_request_ticks=0;
    ble_time_pass_begin(start);
    fm1_ble_irq_restore(key);
    if(!ble_llx_testing) {
        bb_mark_event(BLE_STAGE_WORKER);
        uint32_t item=ble_time_item_begin();
        fm1_ble_service(); ble_time_item_end(item,BLE_WORK_SERVICE,0);
        if(ble_time_budget()) ble_ll_worker(); else ble_port_pend();
        if(ble_time_budget()) ble_worker(); else ble_port_pend();
    }
    key=fm1_ble_irq_save();
    ble_time_pass_end();
    ble_llx_running=0;
    if(!ble_llx_testing) {
        uint32_t delay=ble_llx_pending ? 6u : 6000u;
        if(!ble_llx_pending) chain_start=0;
        else if(!chain_start) chain_start=start;
        else if((uint32_t)(start-chain_start)>=24000000u) {
            delay=60000u; chain_start=0; br_diag.executor_starvation++;
        }
        fm1_ble_worker_timer_arm(delay);
    }
    fm1_ble_irq_restore(key);
}

uint32_t ble_port_ticks(void) { return fm1_ticks(); }
uint32_t ble_port_ms(void) { return fm1_ms; }
int ble_port_start(const ble_adv_t *adv)
{
    uint32_t k=fm1_ble_irq_save(); ble_health_begin(BLE_GAP_RADIO_START); fm1_ble_irq_restore(k);
    int rc=fm1_ble_adv_start(adv);
    if(rc<=0) { k=fm1_ble_irq_save(); ble_health_end(BLE_GAP_RADIO_START); fm1_ble_irq_restore(k); }
    return rc;
}
void ble_port_update(const ble_adv_t *adv) { fm1_ble_adv_update(adv); }
void ble_port_stop_begin(void) { bb.restart=0; fm1_ble_stop_begin(); }
int ble_port_stop_poll(void) { return fm1_ble_stop_poll(); }
int ble_port_failed(void) { return br_diag.error!=0; }
void ble_port_reset_stats(void)
{
    uint32_t k=fm1_ble_irq_save();
    br_diag.adv_events=br_diag.rx_irqs=br_diag.event_irqs=0;
    br_diag.scan_req=br_diag.connect_ignored=br_diag.bad_rx=0;
    br_diag.rx_max_ticks=br_diag.event_max_ticks=br_diag.irq_over_budget=0;
    br_diag.event_work_max_ticks=br_diag.event_over_150us=0;
    br_diag.worker_gap_ticks=0; bb.last_worker=0;
    ble_time_reset();
    fm1_ble_irq_restore(k);
}
static void ble_irq_account(uint32_t t0,int rx)
{
    uint32_t dt=fm1_ticks()-t0;
    uint32_t *max=rx ? &br_diag.rx_max_ticks : &br_diag.event_max_ticks;
    if (dt>*max) *max=dt;
    /* Event total includes optional inline RX; its separate post-RX work is
     * bounded to 150 us in the HAL. RX entry/exit has its own 380 us bound. */
    if (rx && dt>380u*24u) br_diag.irq_over_budget++;

}
void fm1_ble_rx_irq(void)
{
    uint32_t t=fm1_ticks(); fm1_ble_rx_body(); ble_port_pend(); ble_irq_account(t,1);
}
void fm1_ble_event_irq(void)
{
    uint32_t t=fm1_ticks(); fm1_ble_event_body(); ble_port_pend(); ble_irq_account(t,0);
}
/* fm1-base A/B store; calibration remains read-only through plain XIP. */
static int ble_flash_plain(uint32_t off,uint8_t *out,uint32_t n)
{
    return flash_ok ? fm1_ble_flash_read(off,out,n) : -1;
}
int ble_port_storage_idle(void) { return bb.state==0 && !fm1_ble_link_active(); }
static int fm1_ble_flash_allowed(void) { return ble_port_storage_idle(); }
int ble_port_object_load(void *dst,uint32_t n) { return ble_store_valid ? plat_store_load(OBJ_BLE,dst,n) : -1; }
int ble_port_object_save(const void *src,uint32_t n)
{
    if(!ble_store_valid || !ble_port_storage_idle()) return -8;
    if(!flash_ok) return -2;
    fm1_wdt_feed();
    int rc=plat_store_save(OBJ_BLE,src,n);
    fm1_wdt_feed(); return rc;
}
static void ble_platform_irq_probe(void)
{
    uint32_t outer=fm1_ble_irq_save(),before,inner;
    BLE_DIAG_ONLY(br_diag.irq_probe[0]=fm1_ble_icfg_read());
    inner=fm1_ble_irq_save(); BLE_DIAG_ONLY(br_diag.irq_probe[1]=fm1_ble_icfg_read());
    fm1_ble_irq_restore(inner); BLE_DIAG_ONLY(br_diag.irq_probe[2]=fm1_ble_icfg_read());
    before=fm1_ms;
    fm1_delay_us(1200);
    br_diag.irq_ticks_while_masked=fm1_ms-before;
    fm1_ble_irq_restore(outer); BLE_DIAG_ONLY(br_diag.irq_probe[3]=fm1_ble_icfg_read());
    if(br_diag.irq_ticks_while_masked) fm1_ble_fail(9);
}
/* P2 before audio/USB, regardless of APP_BLE_AT_BOOT. */
static void ble_platform_prepare(void)
{
    ble_health_init(fm1_ticks(),fm1_ms); ble_health_begin(BLE_GAP_BOOT);
    ble_breadcrumb_init(&fm1_ble_breadcrumb); bb_mark(BLE_STAGE_PREPARE,0);
    ble_read_cal(ble_flash_plain,&ble_cal);
    ble_store_valid=(uint8_t)fm1_ble_store_map_valid();
    ble_object_init();
    uint32_t r64[2]; fm1_ble_r64_snapshot(r64);
    ble_seed_boot(ble_cal.public_addr,(uint32_t)fm1_boot.p3_rst|(fm1_boot.rst_src<<8),r64[0],r64[1]);
    br_clock_probe();
    ble_health_end(BLE_GAP_BOOT);
}
static int ble_platform_start(void)
{
    /* Calibration runs once, before the executor or any advertising instance.
     * Re-enable never enters this path, even after five off/on cycles. */
    if(ble_started || !ble_port_storage_idle()) return -1;
    ble_health_begin(BLE_GAP_BOOT|BLE_GAP_RADIO_START);
    ble_platform_irq_probe();
    if(!ble_port_failed()) fm1_ble_startup(&ble_cal);
    if(ble_port_failed() || !ble_public_valid(ble_cal.public_addr)) { ble_health_end(BLE_GAP_BOOT|BLE_GAP_RADIO_START); return -1; }
    ble_seed_diag_t seed; ble_seed_get_diag(&seed);
    if(seed.state==BLE_SEED_FAILED) { ble_health_end(BLE_GAP_BOOT|BLE_GAP_RADIO_START); return -1; }
    ble_init(ble_cal.public_addr,0);
    ble_health_ms(fm1_ticks());
    fm1_ble_worker_timer_start();
    uint32_t key=fm1_ble_irq_save(); ble_health_end(BLE_GAP_BOOT|BLE_GAP_RADIO_START); fm1_ble_irq_restore(key);
    if(ble_port_failed()) return -1;
    ble_host_start(); ble_started=1; bb_mark(BLE_STAGE_READY,0); return 0;
}
#pragma clang diagnostic pop
