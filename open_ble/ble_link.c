/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* r4 controller worker. Runs in the dedicated LL executor below audio.
 * ble_ll_poll delivers only to the bounded controller-to-host transport ring. */
#include "ble_link.h"
#include "ble_memory.h"
#include "ble_timing.h"
#include <stddef.h>
#if FELUCCA_BLE_S31
#include "ble_privacy.h"
#endif
#if FELUCCA_BLE_STAGE >= 3
#include "ble_security.h"
#include "ble_seed.h"
#include "ble_crypto.h"
static void security_init(void),security_new(void),security_worker(void);
#if FM1B_BLE_RELIABILITY
static void reliability_disconnect(void),reliability_cancel(void);
static int reliability_hold_close(uint8_t reason),reliability_worker(void),reliability_reset_cleanup(void);
static uint32_t reliability_old_work(void),reliability_work_before_create(void);
static void reliability_created(uint32_t old);
static void reliability_ack(void),reliability_disconnected(void);
#endif
static int security_procedure(const uint8_t *p,uint8_t n);
#endif
struct ble_link ble_link;
typedef struct {
    uint32_t epoch;
    uint16_t header,event;
    uint8_t n,used;
#if FM1B_BLE_DIAG && FELUCCA_BLE_STAGE >= 3
    uint32_t received_ms,since_update_ms;
    uint64_t radio_id;
    uint16_t interval,status;
    uint8_t slot,flags;
#endif
    uint8_t data[BLE_LL_PAYLOAD_MAX];
} rx_t;
typedef struct {
    uint32_t epoch; uint8_t n,llid,state,terminate; uint8_t data[27]; uint32_t order;
#if FM1B_BLE_DIAG && FELUCCA_BLE_STAGE >= 3
    uint64_t radio_counter;
#endif
} tx_t;
static struct {
    rx_t rx[BLE_RX_STREAM],host_rx[BLE_RX_BACKLOG];
    rx_t control;
    uint8_t control_ready,defer_refill,refill_pending;
    uint16_t control_opcode,refill_opcode;
    tx_t tx[BLE_TX_COUNT];
    struct { uint32_t epoch; uint8_t n,data[14]; } meta[16];
    uint8_t mr,mw,mc;
    uint32_t order;
    uint8_t r,w,count,pdus,hr,hw,hcount;
} queues __attribute__((section(".pool.ble")));
static struct { uint32_t epoch; unsigned offset; uint8_t active;
#if FM1B_BLE_RELIABILITY
    uint8_t diag_done;
#endif
} reset;
#if FM1B_BLE_DIAG && FELUCCA_BLE_STAGE >= 3
static uint32_t last_update_ms;
static uint8_t last_update_valid;
#endif

static void control_ignore(uint8_t opcode,uint8_t reason,uint16_t event)
{
    BLE_DIAG_ONLY(ble_link.diag.ignored++); BLE_DIAG_ONLY(ble_link.diag.ignore_reasons[reason]++);
    ble_link.diag.ignored_opcode=opcode; ble_link.diag.ignored_reason=reason;
    ble_link.diag.ignored_event=event;
}
static ble_update_diag_t *update_record(const ble_conn_t *c,uint16_t instant,uint16_t received)
{
#if FM1B_BLE_DIAG
    uint32_t serial=++ble_link.diag.update_serial;
    ble_update_diag_t *d=&ble_link.diag.update[(serial-1u)%4u];
    memset(d,0,sizeof *d); d->serial=serial; d->epoch=ble_link.epoch;
    d->old_interval=ble_link.conn.interval; d->new_interval=c->interval;
    d->win=c->win; d->offset=c->offset; d->instant=instant; d->received=received;
    return d;
#else
    (void)c; (void)instant; (void)received; return 0;
#endif
}
static void update_outcome(uint8_t outcome,uint8_t reason)
{
#if FM1B_BLE_DIAG
    ble_update_diag_t *d;
    if(!ble_link.pending_serial) return;
    d=&ble_link.diag.update[(ble_link.pending_serial-1u)%4u];
    if(d->serial==ble_link.pending_serial) { d->outcome=outcome; d->reason=reason; }
#else
    (void)outcome; (void)reason;
#endif
}
static void instant_clear(void)
{
    memset(&ble_link.pending,0,sizeof ble_link.pending);
    ble_link.instant_kind=ble_link.instant_programmed=ble_link.instant_timed=0;
    ble_link.instant=ble_link.instant_received=0; ble_link.instant_time=ble_link.pending_serial=0;
}
static void timing_clear(void)
{
    instant_clear(); memset(&ble_link.time,0,sizeof ble_link.time);
    ble_link.anchor=ble_link.created_at=ble_link.last_worker=0;
    ble_link.anchor_event=ble_link.closed_n=ble_link.closed_valid=0;
    ble_link.widen=ble_link.window=ble_link.acc=0;
    ble_link.rx_seen=ble_link.valid_seen=ble_link.anchor_seen=ble_link.first_switch=0;
    ble_link.hold=ble_link.priority=ble_link.invalid_events=0;
    ble_link.feature_deadline=ble_link.terminate_deadline=0;
}
static void meta_add(const uint8_t *p,uint8_t n)
{
    if(queues.mc==14) { ble_link.diag.ctrl_drop++; return; } /* 2 reserved: creation/LTK */
    queues.meta[queues.mw].epoch=ble_link.epoch; queues.meta[queues.mw].n=n;
    memcpy(queues.meta[queues.mw].data,p,n); queues.mw=(uint8_t)((queues.mw+1u)%16u); queues.mc++;
}
static void instant_commit(uint32_t now)
{
    if(!ble_link.instant_kind || !ble_link.instant_programmed || (int32_t)(now-ble_link.instant_time)<0) return;
    if(ble_link.instant_kind==1) {
        uint8_t p[12]={0x3e,10,3,0,0x50,0,0,0,0,0,0,0};
        ble_link.conn=ble_link.pending;
        ble_put16(p+6,ble_link.conn.interval); ble_put16(p+8,ble_link.conn.latency); ble_put16(p+10,ble_link.conn.timeout);
        meta_add(p,sizeof p); ble_link.diag.updates++;
#if FM1B_BLE_DIAG && FELUCCA_BLE_STAGE >= 3
        last_update_ms=ble_port_ms()-(uint32_t)(now-ble_link.instant_time)/24000u;
        last_update_valid=1;
#endif
    } else { memcpy(ble_link.conn.map,ble_link.pending.map,5); ble_link.diag.map_updates++; }
    update_outcome(BLE_UPDATE_APPLIED,0); instant_clear();
    ble_link.priority=28; ble_port_priority(28);
}
/* Only form an absolute deadline once the instant is within the signed tick
 * horizon. Rebase from each actual event; never multiply a far instant into
 * a wrapped timestamp. Once armed, a missing I-1 close is caught by the worker. */
static void instant_rebase(void)
{
    uint32_t distance=(uint16_t)(ble_link.instant-ble_link.anchor_event);
    uint32_t step=(uint32_t)ble_link.conn.interval*30000u;
    if(ble_link.instant_kind && !ble_link.instant_programmed && distance<=0x7ffeu &&
       distance<=(0x7fffffffu-BLE_INSTANT_GUARD_US*24u)/step) {
        ble_link.instant_time=ble_link.anchor+distance*step; ble_link.instant_timed=1;
    }
}
static void instant_check(uint32_t now)
{
    if(!ble_link.instant_kind) return;
    if(ble_link.instant_programmed) instant_commit(now);
    else if(ble_link.instant_timed &&
            (int32_t)(ble_link.instant_time-now)<(int32_t)(BLE_INSTANT_GUARD_US*24u)) {
        ble_link.diag.late_instants++; update_outcome(BLE_UPDATE_LATE,0x28); ble_ll_close(0x28);
    }
}
static int live(void) { return !reset.active && (ble_link.state==BLE_LINK_CREATED || ble_link.state==BLE_LINK_UP); }
static void tx_refill(void)
{
    if(queues.defer_refill) {
        queues.refill_pending=2; queues.refill_opcode=queues.control_opcode; ble_port_pend();
    } else ble_port_refill();
}
static void tx_flush(void)
{
    unsigned i;
    for(i=0;i<BLE_TX_COUNT;i++) if(queues.tx[i].state) {
        if(i<5) BLE_DIAG_ONLY(ble_link.diag.flushed++);
        queues.tx[i].state=0;
    }
}
void ble_ll_init(void) { memset(&ble_link,0,sizeof ble_link); memset(&queues,0,sizeof queues);
    memset(&reset,0,sizeof reset);
#if FELUCCA_BLE_STAGE >= 3
    security_init();
#endif
}
void ble_ll_reset(void)
{
    ble_ll_reset_begin();
    while(!ble_ll_reset_step()) {
#if FM1B_BLE_RELIABILITY
        if(ble_reliability_failed()) (void)reliability_worker();
#endif
    } /* synchronous test/startup helper only */
}
void ble_ll_reset_begin(void)
{
    /* No reclaim until stop_poll succeeds. Inhibit worker, delivery, new
     * acceptance and TX picking even if a stop interrupt preempts Reset. */
#if FM1B_BLE_RELIABILITY
    if(!ble_reliability_failed() && (ble_link.state==BLE_LINK_CREATED || ble_link.state==BLE_LINK_UP))
        reliability_disconnect();
#endif
    reset.epoch=ble_link.epoch+1u; reset.offset=0; reset.active=1;
#if FM1B_BLE_RELIABILITY
    reset.diag_done=0;
#endif
    ble_link.state=BLE_LINK_CLOSING;
    timing_clear();
}
int ble_ll_reset_step(void)
{
    unsigned n;
    if(!reset.active) return 1;
#if FM1B_BLE_RELIABILITY
    if(ble_reliability_failed()) return 0;
    if(!reset.diag_done) {
        if(reliability_reset_cleanup()) { reset.diag_done=1; }
        return 0;
    }
#endif
    if(reset.offset<sizeof queues) {
        n=sizeof queues-reset.offset; if(n>128u) n=128u;
        memset((uint8_t *)&queues+reset.offset,0,n); reset.offset+=n; return 0;
    }
    if(reset.offset<sizeof queues+sizeof ble_link) {
        unsigned off=reset.offset-sizeof queues;
        n=sizeof ble_link-off; if(n>128u) n=128u;
        memset((uint8_t *)&ble_link+off,0,n); reset.offset+=n; return 0;
    }
#if FELUCCA_BLE_STAGE >= 3
    security_init(); /* a separate item; cancel output, retain DRBG/rekey */
#endif
    ble_link.epoch=reset.epoch; reset.active=0; return 1;
}
int ble_ll_create(const ble_conn_t *c,uint32_t now)
{
    if(reset.active || ble_link.state!=BLE_LINK_IDLE) return 0;
#if FELUCCA_BLE_S31
    if(!ble_privacy_accept(c)) return 0;
#endif
#if FM1B_BLE_RELIABILITY
    uint32_t old=reliability_work_before_create();
#endif
    /* Preserve only lifetime evidence and epoch, never a previous link's
     * procedure, phase, timing, completion or deferred-queue state. */
    memset((uint8_t *)&ble_link+offsetof(struct ble_link,conn),0,
           sizeof ble_link - offsetof(struct ble_link,conn));
    queues.control_ready=queues.defer_refill=queues.refill_pending=0;
    queues.control_opcode=queues.refill_opcode=0;
    queues.r=queues.w=queues.count=queues.pdus=queues.hr=queues.hw=queues.hcount=0;
    queues.mc=queues.mr=queues.mw=0;
#if FELUCCA_BLE_STAGE >= 3
    security_new(); ble_entropy_connection();
#if FM1B_BLE_RELIABILITY
    reliability_created(old);
#endif
#endif
#if FM1B_BLE_DIAG && FELUCCA_BLE_STAGE >= 3
    last_update_valid=0;
#endif
    ble_link.conn=*c; ble_link.epoch++; ble_link.created_at=now;
    ble_link.state=BLE_LINK_CREATED; ble_link.created_pending=1;
    ble_link.diag.created++; BLE_DIAG_ONLY(ble_link.diag.accepted++);
    ble_link.diag.event=0;
    ble_link.closed_valid=0; ble_link.closed_n=0xffff;
    ble_link.first_switch=1; ble_link.hold=6; ble_link.priority=28;
    ble_link.invalid_events=0; ble_link.instant_programmed=0;
    ble_link.widen=ble_widen(c); ble_link.window=(uint16_t)(c->win*1250u+BLE_FIRST_WINDOW_EXTRA_US);
    ble_link.acc=0; ble_link.rx_seen=ble_link.valid_seen=ble_link.anchor_seen=0;
    ble_link.anchor_event=0; ble_link.reason=0;
    ble_timeline_create(&ble_link.time,c,now); ble_link.anchor=ble_link.time.first;
    ble_link.last_worker=now;
    return 1;
}
void ble_ll_close(uint8_t reason)
{
    uint32_t key=ble_port_lock();
    if(!live()) { ble_port_unlock(key); return; }
#if FM1B_BLE_RELIABILITY
    if(reliability_hold_close(reason)) { ble_port_unlock(key); return; }
    reliability_disconnect();
#endif
#if FELUCCA_BLE_STAGE >= 3
    ble_radio_freeze(reason);
#endif
    ble_link.state=BLE_LINK_CLOSING; ble_link.reason=reason;
    update_outcome(reason==0x28 ? BLE_UPDATE_LATE : BLE_UPDATE_CLOSED,reason);
    timing_clear();
    ble_link.feature_pending=ble_link.features_ready=ble_link.update_pending=0;
    ble_link.peer_stop=ble_link.local_terminate=0;
    queues.control_ready=queues.refill_pending=0; queues.mc=queues.mr=queues.mw=0;
    if(reason==8) ble_link.diag.supervision_fail++;
    if(reason==0x3e) ble_link.diag.establishment_fail++;
    ble_port_stop_begin();
    ble_port_unlock(key);
}
/* Called before returning a hardware slot: 0 means it stays completely full.
 * Copies are small, bounded and preserve the slot's own header and epoch. */
int ble_ll_rx_room(void)
{
    if(live() && (queues.pdus==BLE_RX_BACKLOG || queues.count==BLE_RX_STREAM)) { ble_link.diag.rx_backpressure++; return 0; }
    return 1;
}
int ble_ll_rx_slot(uint16_t status,uint16_t header,const uint8_t *data,uint32_t stamp,uint16_t event,
                   int delayed,uint8_t slot,uint8_t flags)
{
    unsigned cls,n=header>>8;
#if FM1B_BLE_DIAG && FELUCCA_BLE_STAGE >= 3
    uint64_t radio_id=ble_radio_rx(event,status,header,slot,flags);
#endif
    uint8_t reason;
    rx_t *r;
    if(!live()) return 1;
    /* RX progress is a fallback if close IRQs and timed anchors are lost.
     * Use strictly AFTER I: equality can be the idle phase before I with
     * its preceding close still pending (r7's current/just-closed ambiguity). */
    if(ble_link.instant_kind && !ble_link.instant_programmed &&
       (uint16_t)(event-ble_link.instant)>0 && (uint16_t)(event-ble_link.instant)<0x8000u) {
        ble_link.diag.late_instants++; ble_ll_close(0x28); return 1;
    }
    if(!ble_ll_rx_room()) return 0;
    instant_commit(ble_port_ticks());
    if(delayed) BLE_DIAG_ONLY(ble_link.diag.rx_delayed++);
    cls=ble_rx_class(status,(uint8_t)n); BLE_DIAG_ONLY(ble_link.diag.rx_class[cls]++);
    ble_link.rx_seen=1;
    if(cls==0 || cls==2) ble_link.valid_seen=1;
    if(cls==2) BLE_DIAG_ONLY(ble_link.diag.repetitions++);
    reason=ble_timeline_rx_delayed(&ble_link.time,(uint8_t)cls,stamp,ble_port_ticks(),delayed);
    if(reason) { ble_ll_close(reason); return 1; }
    if(ble_link.time.established) {
        ble_link.state=BLE_LINK_UP;
        if(ble_link.first_switch && (uint16_t)(event-ble_link.anchor_event)<0x8000u) {
            ble_link.first_switch=0; ble_link.window=(uint16_t)(2u*ble_link.widen+BLE_WINDOW_PAD_US);
            ble_port_window(ble_link.window);
        }
    }
    /* Anchor from the first timed valid frame of each event. RX timestamp is
     * packet-end service time; the explicit lag constant is a bench retune. */
    if(!delayed && (cls==0 || cls==2) &&
       (uint16_t)(event-ble_link.anchor_event)<0x8000u && (!ble_link.anchor_seen || event!=ble_link.anchor_event)) {
        ble_link.anchor=stamp-(80u+8u*n+BLE_RX_IRQ_LAG_US)*24u;
        ble_link.anchor_event=event; ble_link.anchor_seen=1;
        instant_rebase();
    }
    if(cls || ((header&3u)==1 && !n)) return 1;
#if FELUCCA_BLE_STAGE < 3
    if(!(header&3u) || !n) return 1;
#endif
    r=&queues.rx[queues.w]; r->epoch=ble_link.epoch; r->event=event; r->header=header; r->n=(uint8_t)n;
#if FM1B_BLE_DIAG && FELUCCA_BLE_STAGE >= 3
    r->radio_id=radio_id; r->received_ms=ble_port_ms(); r->interval=ble_link.conn.interval; r->status=status;
    r->since_update_ms=last_update_valid ? r->received_ms-last_update_ms : UINT32_MAX;
    r->slot=slot; r->flags=flags|(delayed ? BLE_RX_DELAYED|BLE_RX_EVENT_UNKNOWN : 0);
    if(cls==2) r->flags|=BLE_RX_REPEAT;
#else
    (void)slot; (void)flags;
#endif
    memcpy(r->data,data,n); r->used=1;
    queues.w=(uint8_t)((queues.w+1u)%BLE_RX_STREAM); queues.count++; queues.pdus++;
    ble_port_pend();
    if((unsigned)queues.count+queues.hcount>ble_link.diag.rx_peak) ble_link.diag.rx_peak=(uint16_t)(queues.count+queues.hcount);
    if((header&3u)!=3 && ble_link.hold<3) ble_link.hold=3;
    return 1;
}
int ble_ll_rx_delayed(uint16_t status,uint16_t header,const uint8_t *data,uint32_t stamp,uint16_t event,int delayed)
{
    return ble_ll_rx_slot(status,header,data,stamp,event,delayed,255,0);
}
int ble_ll_rx_marker(uint8_t marker)
{
    rx_t *r;
    if(!live() || queues.count==BLE_RX_STREAM || !marker) return 0;
#if FM1B_BLE_RELIABILITY
    if(marker==BLE_MARK_RX_ON) reliability_ack();
#endif
    r=&queues.rx[queues.w]; memset(r,0,sizeof *r);
    r->epoch=ble_link.epoch; r->used=(uint8_t)(marker+1u);
    queues.w=(uint8_t)((queues.w+1u)%BLE_RX_STREAM); queues.count++; ble_port_pend(); return 1;
}
int ble_ll_rx(uint16_t status,uint16_t header,const uint8_t *data,uint32_t stamp,uint16_t event)
{
    return ble_ll_rx_delayed(status,header,data,stamp,event,0);
}
static int tx_queue(const uint8_t *p,uint8_t n,uint8_t llid,int end)
{
    unsigned i,start=llid==3 ? (end ? 12u : 5u) : 0u,limit=llid==3 ? (end ? 13u : 12u) : 5u;
#if FELUCCA_BLE_STAGE >= 3
    if(llid==3 && !end && security_procedure(p,n)) { start=13; limit=15; }
#endif
    if(!live() || ble_link.peer_stop) return -1;
    for(i=start;i<limit;i++) if(!queues.tx[i].state) {
        tx_t *t=&queues.tx[i];
        t->epoch=ble_link.epoch; t->n=n; t->llid=llid; t->terminate=(uint8_t)end;
        memcpy(t->data,p,n); t->order=++queues.order; t->state=1;
        if(ble_link.hold<2) ble_link.hold=2;
        ble_port_latency(ble_link.conn.latency,0);
        ble_port_priority(llid==3 ? 30u : 28u);
        tx_refill(); return (int)i;
    }
    if(llid==3) ble_link.diag.ctrl_drop++;
    return -1;
}
#if FELUCCA_BLE_STAGE >= 3
#include "ble_security.inc"
#endif
int ble_ll_acl(const uint8_t *p,uint16_t n)
{
    uint16_t h,len; int rc;
    uint32_t k=ble_port_lock();
    if(n<4) goto reject;
    h=ble_get16(p); len=ble_get16(p+2);
    if(!live() || ble_link.local_terminate || ble_link.peer_stop || (h&0xfffu)!=BLE_HANDLE ||
       (h&0xc000u) || (h&0x3000u)==0x3000u || !len || len>27 || n!=len+4u) goto reject;
    rc=tx_queue(p+4,(uint8_t)len,(h&0x1000u) ? 1u : 2u,0);
    if(rc<0) goto reject;
    BLE_DIAG_ONLY(ble_link.diag.acl_in++); ble_port_unlock(k); return 0;
reject:
    ble_link.diag.acl_rejected++; ble_port_unlock(k); return 1;
}
int ble_ll_acl_waiting(void)
{
    unsigned i;
    for(i=0;i<5;i++) if(queues.tx[i].state==1) return 1;
    return 0;
}
int ble_ll_tx_pick(void)
{
    unsigned i; int best=-1;
    if(!live() || ble_link.peer_stop) return -1;
#if FELUCCA_BLE_STAGE >= 3
    if(security.d.tx_on && security.d.tx_counter>=BLE_CCM_LAST && !ble_link.local_terminate)
        security_terminate(0x13);
    if(!live()) return -1;
    if(queues.tx[12].state==1) best=12;
    for(i=13;best<0 && i<15;i++) if(queues.tx[i].state==1 && security_tx_allowed(&queues.tx[i])) {
        unsigned other=i==13 ? 14 : 13;
        best=(int)i;
        if(queues.tx[other].state==1 && security_tx_allowed(&queues.tx[other]) &&
            (int32_t)(queues.tx[other].order-queues.tx[i].order)<0) best=(int)other;
    }
    if(best<0 && security.d.tx_counter<BLE_CCM_LAST)
        for(i=5;i<12;i++) if(queues.tx[i].state==1 && security_tx_allowed(&queues.tx[i]) &&
            (best<0 || (int32_t)(queues.tx[i].order-queues.tx[best].order)<0)) best=(int)i;
    if(best<0 && security.d.tx_counter<BLE_CCM_LAST)
        for(i=0;i<5;i++) if(queues.tx[i].state==1 && security_tx_allowed(&queues.tx[i]) &&
            (best<0 || (int32_t)(queues.tx[i].order-queues.tx[best].order)<0)) best=(int)i;
#else
    for(i=5;i<BLE_TX_COUNT;i++) if(queues.tx[i].state==1 &&
        (best<0 || (int32_t)(queues.tx[i].order-queues.tx[best].order)<0)) best=(int)i;
    if(best<0) for(i=0;i<5;i++) if(queues.tx[i].state==1 &&
        (best<0 || (int32_t)(queues.tx[i].order-queues.tx[best].order)<0)) best=(int)i;
#endif
#if FM1B_BLE_RELIABILITY
    best=reliability_tx_pick(best);
#endif
    if(best>=0) queues.tx[best].state=2;
    return best;
}
const uint8_t *ble_ll_tx_data(unsigned id) { return queues.tx[id].data; }
uint8_t ble_ll_tx_len(unsigned id) { return queues.tx[id].n; }
uint8_t ble_ll_tx_llid(unsigned id) { return queues.tx[id].llid; }
void ble_ll_tx_ack(unsigned id)
{
    tx_t *t=&queues.tx[id];
    if(t->state!=2 || t->epoch!=ble_link.epoch) return;
    t->state=0;
    if(id<5) { ble_link.completed++; BLE_DIAG_ONLY(ble_link.diag.acknowledged++); }
    if(t->terminate && live()) ble_ll_close(0x16);
}
uint8_t ble_ll_disconnect(uint16_t handle,uint8_t reason)
{
    static const uint8_t allowed[]={5,0x13,0x14,0x15,0x1a,0x29,0x3b};
    unsigned i; uint8_t p[2]={2,reason};
    if(handle!=BLE_HANDLE || ble_link.state==BLE_LINK_IDLE) return 2;
    if(!live() || ble_link.local_terminate || ble_link.peer_stop) return 0x0c;
    for(i=0;i<sizeof allowed;i++) if(allowed[i]==reason) break;
    if(i==sizeof allowed) return 0x12;
    if(tx_queue(p,2,3,1)<0) return 0x0c;
    ble_link.local_terminate=1;
    ble_link.terminate_deadline=ble_port_ticks()+(uint32_t)ble_link.conn.timeout*240000u;
    return 0;
}
uint8_t ble_ll_features(uint16_t handle)
{
    if(handle!=BLE_HANDLE || !live()) return 2;
    if(ble_link.feature_pending || ble_link.features_ready) return 0x0c;
    ble_link.feature_pending=1; ble_link.feature_deadline=ble_port_ticks()+800u*24000u;
    return 0;
}
static void control_plain(const uint8_t *p,uint8_t n,uint16_t received)
{
    static const uint8_t sizes[]={12,8,2,23,13,1,1,2,9,9,1,1,6,2,9,24,24,3,1,1,9,9};
    uint8_t reply[9]={0}; unsigned op=p[0];
    if(op<sizeof sizes && n!=sizes[op]) { control_ignore((uint8_t)op,BLE_IGNORE_LENGTH,received); return; }
    switch(op) {
    case 0: case 1: {
        ble_conn_t c=ble_link.conn; uint16_t instant;
        ble_update_diag_t *record=0;
        if(!op) {
            c.win=p[1]; c.offset=ble_get16(p+2); c.interval=ble_get16(p+4);
            c.latency=ble_get16(p+6); c.timeout=ble_get16(p+8); instant=ble_get16(p+10);
            record=update_record(&c,instant,received);
        } else {
            memcpy(c.map,p+1,5); c.map[4]&=31; instant=ble_get16(p+6);
        }
        if(ble_link.instant_kind) {
            if(record) record->outcome=BLE_UPDATE_BUSY;
            control_ignore((uint8_t)op,BLE_IGNORE_PENDING,received); return;
        }
        /* Core Vol 6 B 5.1.1: window limits use connInterval_NEW.
         * Invalid parameters follow r7 03 section 7: ignore, never reject. */
        if((!op && !ble_conn_params_valid(&c)) || (op && ble_map_count(c.map)<2)) {
            if(record) record->outcome=BLE_UPDATE_INVALID;
            control_ignore((uint8_t)op,op ? BLE_IGNORE_MAP : BLE_IGNORE_PARAMS,received); return;
        }
        if(!ble_instant_valid(instant,ble_link.closed_valid ? ble_link.closed_n : 0xffffu)) {
            if(record) { record->outcome=BLE_UPDATE_LATE; record->reason=0x28; }
            ble_link.diag.late_instants++; ble_ll_close(0x28); return;
        }
        ble_link.pending=c; ble_link.instant=instant; ble_link.instant_kind=(uint8_t)(op+1u);
        ble_link.instant_received=received; ble_link.instant_programmed=ble_link.instant_timed=0;
        if(record) { record->outcome=BLE_UPDATE_PENDING; ble_link.pending_serial=record->serial; }
        instant_rebase(); instant_check(ble_port_ticks());
        if(!live()) return;
        ble_port_latency(ble_link.conn.latency,0);
        ble_port_instant((uint8_t)(op+1u),instant,&c); break;
    }
    case 2: ble_link.peer_stop=1; ble_link.reason=p[1];
        ble_link.feature_pending=ble_link.features_ready=0; queues.mc=queues.mr=queues.mw=0; break;
    case 8:
        memcpy(ble_link.features,p+1,8); ble_link.features[0]&=FELUCCA_BLE_STAGE>=3 ? 1u : 0u;
        ble_link.features_known=1; reply[0]=9; reply[1]=FELUCCA_BLE_STAGE>=3 ? 1u : 0u; tx_queue(reply,9,3,0); break;
    case 12:
        if(ble_link.version_known) { control_ignore(12,BLE_IGNORE_VERSION,received); break; }
        if(!ble_link.version_known) { memcpy(ble_link.peer_version,p+1,5); ble_link.version_known=1; }
        if(!ble_link.version_sent) {
            reply[0]=12; reply[1]=6; ble_put16(reply+2,0xffff); ble_put16(reply+4,2);
            if(tx_queue(reply,6,3,0)>=0) ble_link.version_sent=1;
        }
        break;
    case 3: /* S2 advertises no encryption: reject the unsupported procedure. */
        reply[0]=13; reply[1]=0x1a; tx_queue(reply,2,3,0); break;
    case 4: case 5: case 13: case 14: case 16: case 17:
        control_ignore((uint8_t)op,BLE_IGNORE_ROLE,received); break;
    case 6: case 7: case 9: case 10: case 11: case 19:
        control_ignore((uint8_t)op,BLE_IGNORE_UNEXPECTED,received); break;
    default: reply[0]=7; reply[1]=(uint8_t)op; tx_queue(reply,2,3,0); break;
    }
}
static void control(const uint8_t *p,uint8_t n,uint16_t received)
{
    uint32_t key;
#if FELUCCA_BLE_STAGE >= 3
    if(security_control(p,n,received)) return;
#endif
    key=ble_port_lock(); control_plain(p,n,received); ble_port_unlock(key);
}
void ble_ll_event(uint16_t event,uint32_t now)
{
#if FELUCCA_BLE_STAGE >= 3
    ble_radio_event(event,1);
#endif
#if FM1B_BLE_RELIABILITY
    if(ble_reliability_failed()) return;
#endif
    uint8_t reason; unsigned i; int pending=0;
    if(!live() || (!ble_link.closed_valid && (int32_t)(now-ble_link.time.first)<0)) return; /* A8 only before first close */
    if(ble_link.closed_valid && ((uint16_t)(event-ble_link.closed_n)==0 ||
                                (uint16_t)(event-ble_link.closed_n)>=0x8000u)) return;
    ble_link.diag.events++; ble_link.diag.event=event;
    instant_commit(now);
    if(event!=ble_link.anchor_event) {
        ble_link.anchor+=(uint32_t)(uint16_t)(event-ble_link.anchor_event)*ble_link.conn.interval*30000u;
        ble_link.anchor_event=event;
    }
    ble_link.closed_n=event; ble_link.closed_valid=1;
    if(ble_link.peer_stop) { ble_ll_close(ble_link.reason); return; }
    if(ble_link.instant_kind && !ble_link.instant_programmed &&
       (uint16_t)(event-(uint16_t)(ble_link.instant-1u))<0x8000u && (uint16_t)(event+1u)!=ble_link.instant) {
        /* Never leave a procedure pending after a skipped I-1 close. */
        ble_link.diag.late_instants++; ble_ll_close(0x28); return;
    }
    instant_rebase();
    if(ble_link.instant_kind && !ble_link.instant_programmed && (uint16_t)(event+1u)==ble_link.instant) {
        uint8_t kind=ble_link.instant_kind;
        ble_link.instant_time=ble_link.anchor+(uint32_t)(uint16_t)(ble_link.instant-ble_link.anchor_event)*ble_link.conn.interval*30000u;
        if((int32_t)(ble_link.instant_time-now)<(int32_t)(BLE_INSTANT_GUARD_US*24u)) {
            ble_link.diag.late_instants++; ble_ll_close(0x28); return;
        }
        if(kind==1) {
            ble_conn_t step=ble_link.conn;
            uint32_t ti=ble_link.anchor+(uint32_t)(uint16_t)(ble_link.instant-ble_link.anchor_event)*ble_link.conn.interval*30000u;
            uint32_t extra; uint16_t wi;
            { static const uint16_t ppm[]={500,250,150,100,75,50,30,20};
              uint32_t product=(uint32_t)(ppm[step.sca&7u]+BLE_LOCAL_SCA_PPM)*(step.interval+ble_link.pending.offset);
              wi=(uint16_t)(((product/800u)+1u)*2u); }
            extra=ble_link.widen>wi ? (uint32_t)ble_link.widen+wi : 0;
            ble_link.widen=ble_widen(&ble_link.pending);
            if(ble_link.widen>BLE_UPDATE_WIDEN_US) ble_link.widen=BLE_UPDATE_WIDEN_US;
            ble_link.window=(uint16_t)(extra+ble_link.pending.win*1250u+BLE_UPDATE_WINDOW_EXTRA_US);
            ble_timeline_arm(&ble_link.time,ti,ti+(uint32_t)ble_link.pending.offset*30000u,ble_link.pending.timeout);
            ble_link.anchor=ble_link.time.win_reset; ble_link.anchor_event=ble_link.instant; ble_link.anchor_seen=0;
            ble_link.first_switch=1; ble_link.acc=0;
        }
        ble_port_apply(kind,&ble_link.pending,ble_link.widen,ble_link.window);
        ble_link.instant_programmed=1;
        update_outcome(BLE_UPDATE_PROGRAMMED,0);
    }
    /* During event I, apply the new supervision period, even before its close. */
    reason=ble_timeline_check(&ble_link.time,ble_link.instant_kind==1 && ble_link.instant_programmed ?
        &ble_link.pending : &ble_link.conn,now);
    if(reason) { ble_ll_close(reason); return; }
    for(i=0;i<BLE_TX_COUNT;i++) if(queues.tx[i].state) { pending=1; break; }
    if(!ble_link.valid_seen) pending=1;
    if(pending && ble_link.hold<1) ble_link.hold=1;
    if(ble_link.hold) ble_link.hold--;
    /* Reaching zero after a hold=1 must not enable latency while an owned
     * TX slot is still waiting for its acknowledgement. */
    ble_port_latency(ble_link.instant_programmed ? ble_link.pending.latency : ble_link.conn.latency,
                     !ble_link.instant_kind && ble_link.hold==0 && !pending);
    ble_link.invalid_events=ble_link.valid_seen ? 0 : (uint8_t)(ble_link.invalid_events<254 ? ble_link.invalid_events+1u : 254u);
    if(ble_link.instant_kind || ble_link.invalid_events>3) ble_link.priority=30;
    else if(ble_ll_acl_waiting()) ble_link.priority=28;
    else if(ble_link.priority!=30 || ble_link.valid_seen) {
        unsigned p=ble_link.priority ? ble_link.priority-1u : 19u;
        if(p>27) p=27;
        if(p==25) p=19;
        if(p<20) p=ble_link.invalid_events ? 28 : 19;
        ble_link.priority=(uint8_t)p;
    }
    ble_port_priority(ble_link.priority);
    /* The HAL consumes rx_seen/valid_seen for missed-event correction then
     * clears them; it never uses RX status bit 8 for these indications. */
}
void ble_ll_worker(void)
{
    /* One control PDU can use decode, dispatch and two one-slot refills.
     * Keep the old PDU drain capacity; own/pass clocks bound actual work. */
    unsigned budget=4u*BLE_RX_BACKLOG;
    uint32_t start=ble_port_ticks(),own_start=ble_time_own(),gap=start-ble_link.last_worker,key,item;
#if FM1B_BLE_RELIABILITY
    if(ble_time_budget() && reliability_worker()) return;
    if(!ble_time_budget()) { ble_port_pend(); return; }
#endif
    if(reset.active) return;
    if(live()) {
        BLE_DIAG_ONLY(ble_link.diag.worker_calls++);
        if(gap>ble_link.diag.worker_gap_ticks) ble_link.diag.worker_gap_ticks=gap;
        if(gap>7500u*24u) ble_link.diag.worker_over_gap++;
    }
    ble_link.last_worker=start;
#if FELUCCA_BLE_STAGE >= 3
    item=ble_time_item_begin(); ble_entropy_poll(); ble_time_item_end(item,BLE_WORK_ENTROPY_SAMPLE,0);
    if(ble_time_budget()) { item=ble_time_item_begin(); security_worker(); ble_time_item_end(item,BLE_WORK_KEY,0); }
#endif
    while(budget-- && ble_time_budget() && (uint32_t)(ble_time_own()-own_start)<100u*24u) {
        rx_t local; ble_work_kind_t kind=BLE_WORK_RX;
        uint16_t opcode=0xffffu; /* unavailable on an unauthenticated control */
        item=ble_time_item_begin();
        if(queues.refill_pending) {
            opcode=queues.refill_opcode; queues.refill_pending--;
            if(live() && !ble_link.peer_stop) ble_port_refill_one();
            ble_time_item_end(item,BLE_WORK_REFILL,opcode); continue;
        }
        if(queues.control_ready) {
            local=queues.control; queues.control_ready=0; opcode=queues.control_opcode;
            queues.defer_refill=1;
            if(live() && !ble_link.peer_stop && local.epoch==ble_link.epoch) {
                if(local.used>1) {
                    BLE_DIAG_ONLY(ble_link.diag.epoch_markers++);
#if FELUCCA_BLE_STAGE >= 3
                    security_marker((uint8_t)(local.used-1u));
#endif
                } else control(local.data,local.n,local.event);
            }
            queues.defer_refill=0;
            ble_time_item_end(item,BLE_WORK_CONTROL,opcode); continue;
        }
        key=ble_port_lock();
        if(!live() || ble_link.peer_stop || !queues.count) { ble_port_unlock(key); ble_time_item_end(item,kind,0); break; }
        local=queues.rx[queues.r];
        if(local.used==1 && (local.header&3u)!=3 && queues.hcount==BLE_RX_BACKLOG) { ble_port_unlock(key); ble_time_item_end(item,kind,0); break; }
        queues.r=(uint8_t)((queues.r+1u)%BLE_RX_STREAM); queues.count--;
        if(local.used==1) queues.pdus--;
        ble_port_unlock(key);
        kind=(local.used>1 || (local.header&3u)==3) ? BLE_WORK_CONTROL_RX : BLE_WORK_RX;
        if(local.epoch==ble_link.epoch) {
            if(local.used>1) opcode=(uint16_t)(0x100u+local.used-1u);
#if FELUCCA_BLE_STAGE >= 3
            else if(!security_receive(&local)) { ble_time_item_end(item,kind,opcode); continue; }
#endif
            if(local.used>1 || (local.header&3u)==3) {
                /* No later PDU/marker is consumed until dispatch finishes.
                 * Encryption changes and the first reply's queue insertion
                 * still share security_control's original critical section. */
                if(local.used==1 && local.n) opcode=local.data[0];
                queues.control=local; queues.control_opcode=opcode; queues.control_ready=1;
            } else {
                key=ble_port_lock(); queues.host_rx[queues.hw]=local; queues.hw=(uint8_t)((queues.hw+1u)%BLE_RX_BACKLOG);
                queues.hcount++; ble_port_unlock(key);
            }
        }
        ble_time_item_end(item,kind,opcode);
    }
    if(!ble_time_budget()) { ble_port_pend(); goto finish; }
    item=ble_time_item_begin();
    key=ble_port_lock();
    if(live()) {
        instant_check(ble_port_ticks());
        uint8_t reason;
        ble_port_rx_recover();
        reason=ble_timeline_check(&ble_link.time,ble_link.instant_kind==1 && ble_link.instant_programmed ?
            &ble_link.pending : &ble_link.conn,ble_port_ticks());
        if(reason) ble_ll_close(reason);
        else if(ble_link.local_terminate && (int32_t)(ble_port_ticks()-ble_link.terminate_deadline)>=0) { ble_link.diag.terminate_timeouts++; ble_ll_close(8); }
        if(live() && ble_link.feature_pending && (ble_link.features_known || (int32_t)(ble_port_ticks()-ble_link.feature_deadline)>=0)) {
            ble_link.feature_pending=0; ble_link.features_ready=1;
            ble_link.features_status=ble_link.features_known ? 0 : 0x1a;
            { uint8_t p[14]={0x3e,12,4,0,0x50,0,0,0,0,0,0,0,0,0};
              p[3]=ble_link.features_status;
              if(!p[3]) memcpy(p+6,ble_link.features,8);
              meta_add(p,sizeof p); ble_link.features_ready=0; }
        }
    }
    ble_port_unlock(key);
    if(ble_link.state==BLE_LINK_CLOSING && ble_port_stop_poll()) {
        key=ble_port_lock(); tx_flush(); queues.count=queues.pdus=queues.hcount=0; queues.mc=queues.mr=queues.mw=0;
#if FELUCCA_BLE_STAGE >= 3
        security_new();
#endif
        ble_link.state=BLE_LINK_STOPPED; ble_link.disconn_pending=1; ble_port_unlock(key);
    }
    ble_time_item_end(item,BLE_WORK_HOUSEKEEP,0);
finish:
    gap=ble_port_ticks()-start;
    if(gap>ble_link.diag.worker_max_ticks) ble_link.diag.worker_max_ticks=gap;
    if(queues.count || queues.control_ready || queues.refill_pending) ble_port_pend();
}
/* All host data originate here, called only by the executor HCI worker. */
int ble_ll_poll(uint8_t *type,uint8_t *p,uint16_t *n)
{
    uint32_t key=ble_port_lock();
    if(reset.active) { ble_port_unlock(key); return 0; }
#if FELUCCA_BLE_S31
    /* Acceptance may interrupt immediately before lock acquisition. Observe
     * creation under the lock; never format an event for an unresolved new
     * connection after checking created_pending outside this section. */
    if(ble_link.created_pending && !ble_link.resolve_done) {
        ble_port_unlock(key);
        (void)ble_privacy_resolve_step(); ble_port_pend(); return 0;
    }
#endif
    *type=4;
    if(ble_link.created_pending) {
        const ble_conn_t *c=&ble_link.conn;
        p[0]=0x3e; p[1]=19; p[2]=1; p[3]=0; ble_put16(p+4,BLE_HANDLE);
        p[6]=1; p[7]=c->peer_type; memcpy(p+8,c->peer,6);
#if FELUCCA_BLE_S31
        /* r6: legacy always reports the address used on air. Enhanced is
         * formatted by the controller, where the LE event mask is owned. */
#endif
        ble_put16(p+14,c->interval); ble_put16(p+16,c->latency); ble_put16(p+18,c->timeout); p[20]=c->sca;
        *n=21; ble_link.created_pending=0;
    } else if(ble_link.completed) {
        p[0]=0x13; p[1]=5; p[2]=1; ble_put16(p+3,BLE_HANDLE); ble_put16(p+5,ble_link.completed);
        BLE_DIAG_ONLY(ble_link.diag.ncp+=ble_link.completed); ble_link.completed=0; *n=7;
    } else if(ble_link.disconn_pending) {
        p[0]=5; p[1]=4; p[2]=0; ble_put16(p+3,BLE_HANDLE); p[5]=ble_link.reason; *n=6;
#if FM1B_BLE_RELIABILITY
        reliability_disconnected();
#endif
        ble_link.disconn_pending=0; ble_link.diag.disconnects++; ble_link.diag.reason=ble_link.reason;
        ble_link.state=BLE_LINK_IDLE; ble_link.epoch++;
        ble_link.local_terminate=ble_link.features_known=ble_link.version_sent=ble_link.version_known=0;
        queues.r=queues.w=queues.hr=queues.hw=0;
#if FELUCCA_BLE_STAGE >= 3
    } else if(security_poll(p,n)) {
#endif
    } else if(live() && queues.mc) {
        *n=queues.meta[queues.mr].n; memcpy(p,queues.meta[queues.mr].data,*n);
        queues.mr=(uint8_t)((queues.mr+1u)%16u); queues.mc--;
    } else if(live() && queues.hcount) {
        rx_t *r=&queues.host_rx[queues.hr];
        *type=2; ble_put16(p,(uint16_t)(BLE_HANDLE|((r->header&3u)==2 ? 0x2000u : 0x1000u)));
        ble_put16(p+2,r->n); memcpy(p+4,r->data,r->n); *n=(uint16_t)(r->n+4u);
        queues.hr=(uint8_t)((queues.hr+1u)%BLE_RX_BACKLOG); queues.hcount--; BLE_DIAG_ONLY(ble_link.diag.acl_out++);
    } else { ble_port_unlock(key); return 0; }
    ble_port_unlock(key); return 1;
}
void ble_ll_get_diag(ble_link_diag_t *d)
{
    uint32_t k=ble_port_lock(); unsigned i;
    *d=ble_link.diag; d->state=ble_link.state; d->epoch=ble_link.epoch;
    d->interval=ble_link.conn.interval; d->latency=ble_link.conn.latency; d->timeout=ble_link.conn.timeout;
    d->established=ble_link.time.established;
    d->pending=ble_link.pending; d->instant=ble_link.instant; d->received=ble_link.instant_received;
    d->instant_kind=ble_link.instant_kind; d->programmed=ble_link.instant_programmed;
    d->rx_used=(uint16_t)(queues.count+queues.hcount); d->tx_used=0;
    for(i=0;i<BLE_TX_COUNT;i++) d->tx_used+=(uint16_t)(queues.tx[i].state!=0);
    memcpy(d->peer_features,ble_link.features,8); ble_port_unlock(k);
}
int ble_ll_pending(void)
{
    if(reset.active) return 0;
#if FM1B_BLE_RELIABILITY
    /* Failure classification closes in a separate item. Keep the stop and
     * Disconnection Complete path scheduled after that item returns. */
    if(ble_link.state==BLE_LINK_CLOSING) return 1;
#endif
    return ble_link.created_pending || ble_link.disconn_pending || ble_link.completed ||
        (live() && (queues.count || queues.hcount || queues.mc || queues.control_ready || queues.refill_pending
#if FELUCCA_BLE_STAGE >= 3
            || security.ltk_event || security.encryption_event || security.reply_pending || security.random_pending
#if FM1B_BLE_RELIABILITY
            || ble_reliability_pending()
#endif
#endif
        ));
}

#if FELUCCA_BLE_STAGE >= 3
int ble_ll_rx_reserve(unsigned pdus,unsigned markers)
{
    return live() && queues.pdus+pdus<=BLE_RX_BACKLOG && queues.count+pdus+markers<=BLE_RX_STREAM;
}
#endif
