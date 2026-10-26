/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* r5 04 §0.8. All accumulator observations are atomic with TIMER4 reads. */
#include "ble_link.h"
#include "ble_timing.h"
#include "ble_memory.h"
static struct {
    ble_timing_diag_t d;
    volatile uint32_t preempt_start,preempt_total,depth,ccm;
    uint32_t pass_wall,pass_own,pass_item;
    uint8_t running;
} tm;
static void maximum(uint32_t *p,uint32_t v) { if(v>*p) *p=v; }
void ble_time_preempt_enter(void)
{
    uint32_t k=ble_port_lock();
    if(!tm.depth++) tm.preempt_start=ble_port_ticks();
    ble_port_unlock(k);
}
void ble_time_preempt_leave(void)
{
    uint32_t k=ble_port_lock();
    if(tm.depth && !--tm.depth) tm.preempt_total+=ble_port_ticks()-tm.preempt_start;
    ble_port_unlock(k);
}
uint32_t ble_time_own(void)
{
    uint32_t k=ble_port_lock(),now=ble_port_ticks();
    uint32_t v=now-tm.preempt_total-(tm.depth ? now-tm.preempt_start : 0);
    ble_port_unlock(k); return v;
}
void ble_time_dispatch(uint32_t request,uint32_t entry)
{
    int32_t delta=(int32_t)(entry-request);
    if(delta<0) tm.d.dispatch_negative++;
    else {
        maximum(&tm.d.dispatch_max,(uint32_t)delta);
        if(ble_link.state!=BLE_LINK_IDLE && ble_link.state!=BLE_LINK_STOPPED)
            maximum(&tm.d.dispatch_live_max,(uint32_t)delta);
    }
}
void ble_time_pass_begin(uint32_t wall)
{
    tm.pass_wall=wall; tm.pass_own=wall-tm.preempt_total; tm.pass_item=0; tm.running=1;
}
void ble_time_pass_end(void)
{
    uint32_t own=ble_time_own()-tm.pass_own;
    maximum(&tm.d.wall_max,ble_port_ticks()-tm.pass_wall); maximum(&tm.d.own_max,own);
    if(own>250u*24u) tm.d.own_over_250us++;
    if(own>250u*24u+tm.pass_item) tm.d.pass_over_budget++;
    tm.running=0;
}
int ble_time_budget(void) { return !tm.running || (uint32_t)(ble_time_own()-tm.pass_own)<250u*24u; }
#if FM1B_BLE_RELIABILITY
int ble_time_has_room(uint32_t ticks)
{ return !tm.running || (uint32_t)(ble_time_own()-tm.pass_own)+ticks<250u*24u; }
#endif
uint32_t ble_time_item_begin(void) { return ble_time_own(); }
void ble_time_item_end(uint32_t start,ble_work_kind_t kind,uint16_t opcode)
{
    uint32_t dt=ble_time_own()-start;
    if(!tm.running || kind>=BLE_WORK_COUNT) return;
    BLE_DIAG_ONLY(tm.d.kind_count[kind]++);
    if(dt>tm.d.item_max) { tm.d.item_max=dt; tm.d.item_kind=(uint8_t)kind; tm.d.item_opcode=opcode; }
    BLE_DIAG_ONLY(if(dt>tm.d.kind_max[kind]) { tm.d.kind_max[kind]=dt; tm.d.kind_opcode[kind]=opcode; });
    maximum(&tm.pass_item,dt);
    if(dt>100u*24u) { tm.d.item_over_100us++; BLE_DIAG_ONLY(tm.d.kind_over_100us[kind]++); }
}
const char *ble_time_kind_name(unsigned kind)
{
#if FM1B_BLE_DIAG
    static const char *const names[BLE_WORK_COUNT]={"service","key","rx","control",
        "housekeep","delivery","resolution","driver","command","stop","adv_prepare",
        "entropy_sample","entropy_reseed","entropy_output","entropy_rekey",
        "control_rx","refill","reset_clear","reset_defaults","reset_stats","reset_complete"
#if FM1B_BLE_RELIABILITY
        ,"reliability"
#endif
    };
    return kind<BLE_WORK_COUNT ? names[kind] : "unknown";
#else
    (void)kind; return "disabled";
#endif
}
void ble_time_ccm_add(uint32_t ticks) { uint32_t k=ble_port_lock(); tm.ccm+=ticks; ble_port_unlock(k); }
uint32_t ble_time_ccm_total(void) { return tm.ccm; }
void ble_time_refill_end(uint32_t start,uint32_t ccm_before)
{
    uint32_t total=ble_port_ticks()-start,ccm=tm.ccm-ccm_before;
    maximum(&tm.d.refill_max,total); maximum(&tm.d.refill_ccm_max,ccm);
    maximum(&tm.d.refill_other_max,total>=ccm ? total-ccm : 0);
    if(total>80u*24u) tm.d.refill_over_80us++;
}
void ble_time_get_diag(ble_timing_diag_t *out) { uint32_t k=ble_port_lock(); *out=tm.d; ble_port_unlock(k); }
void ble_time_reset(void) { uint32_t k=ble_port_lock(); memset(&tm.d,0,sizeof tm.d); ble_port_unlock(k); }
