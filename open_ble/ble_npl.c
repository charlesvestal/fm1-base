/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#include <string.h>
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "ble_s1.h"
#include "ble_host.h"
#include "ble_link.h"
static struct ble_npl_eventq default_q;
static struct ble_npl_callout *callouts;
static unsigned critical_depth;
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void) { return &default_q; }
bool ble_npl_os_started(void) { return true; }
void *ble_npl_get_current_task_id(void) { return &default_q; }
void ble_npl_eventq_init(struct ble_npl_eventq *q) { memset(q,0,sizeof *q); }
struct ble_npl_event *ble_npl_eventq_get(struct ble_npl_eventq *q,ble_npl_time_t timeout)
{
    struct ble_npl_event *e=q->head; (void)timeout;
    if(e) { q->head=e->next; if(!q->head) q->tail=0; e->next=0; e->queued=false; }
    return e;
}
void ble_npl_eventq_put(struct ble_npl_eventq *q,struct ble_npl_event *e)
{
    if(e->queued) return;
    e->next=0; e->queued=true;
    if(q->tail) q->tail->next=e; else q->head=e;
    q->tail=e;
}
void ble_npl_eventq_remove(struct ble_npl_eventq *q,struct ble_npl_event *e)
{
    struct ble_npl_event *p=q->head,*prev=0;
    while(p && p!=e) { prev=p; p=p->next; }
    if(!p) return;
    if(prev) prev->next=p->next; else q->head=p->next;
    if(q->tail==p) q->tail=prev;
    p->queued=false; p->next=0;
}
void ble_npl_event_init(struct ble_npl_event *e,ble_npl_event_fn *fn,void *arg) { memset(e,0,sizeof *e); e->fn=fn; e->arg=arg; }
bool ble_npl_event_is_queued(struct ble_npl_event *e) { return e->queued; }
void *ble_npl_event_get_arg(struct ble_npl_event *e) { return e->arg; }
void ble_npl_event_set_arg(struct ble_npl_event *e,void *arg) { e->arg=arg; }
bool ble_npl_eventq_is_empty(struct ble_npl_eventq *q) { return !q->head; }
void ble_npl_event_run(struct ble_npl_event *e) { if(e && e->fn) e->fn(e); }
ble_npl_error_t ble_npl_mutex_init(struct ble_npl_mutex *m) { m->depth=0; return 0; }
ble_npl_error_t ble_npl_mutex_pend(struct ble_npl_mutex *m,ble_npl_time_t timeout) { (void)timeout; m->depth++; return 0; }
ble_npl_error_t ble_npl_mutex_release(struct ble_npl_mutex *m) { if(!m->depth) return BLE_NPL_BAD_MUTEX; m->depth--; return 0; }
ble_npl_error_t ble_npl_sem_init(struct ble_npl_sem *s,uint16_t tokens) { s->tokens=tokens; return 0; }
ble_npl_error_t ble_npl_sem_pend(struct ble_npl_sem *s,ble_npl_time_t timeout)
{
    uint32_t start=ble_port_ms();
    /* NimBLE's synchronous HCI ACK wait. Only the controller and transport
     * ACK path are pumped; never recurse into a host event or a callout. */
    do {
        if(s->tokens) { s->tokens--; return 0; }
        ble_host_wait_step();
    } while((uint32_t)(ble_port_ms()-start)<timeout);
    return BLE_NPL_TIMEOUT;
}
ble_npl_error_t ble_npl_sem_release(struct ble_npl_sem *s) { if(s->tokens==UINT16_MAX) return BLE_NPL_EBUSY; s->tokens++; return 0; }
uint16_t ble_npl_sem_get_count(struct ble_npl_sem *s) { return s->tokens; }
void ble_npl_callout_init(struct ble_npl_callout *c,struct ble_npl_eventq *q,ble_npl_event_fn *fn,void *arg)
{
    struct ble_npl_callout *p;
    /* Init may repeat after host Reset: do not insert the same node twice. */
    for(p=callouts;p;p=p->next) if(p==c) break;
    if(!p) { c->next=callouts; callouts=c; }
    ble_npl_event_init(&c->ev,fn,arg); c->q=q; c->active=false;
}
ble_npl_error_t ble_npl_callout_reset(struct ble_npl_callout *c,ble_npl_time_t ticks)
{
    if(c->q) ble_npl_eventq_remove(c->q,&c->ev);
    c->deadline=ble_port_ms()+(ticks ? ticks : 1u); c->active=true; return 0;
}
void ble_npl_callout_stop(struct ble_npl_callout *c) { c->active=false; if(c->q) ble_npl_eventq_remove(c->q,&c->ev); }
bool ble_npl_callout_is_active(struct ble_npl_callout *c) { return c->active; }
ble_npl_time_t ble_npl_callout_get_ticks(struct ble_npl_callout *c) { return c->deadline; }
ble_npl_time_t ble_npl_callout_remaining_ticks(struct ble_npl_callout *c,ble_npl_time_t now) { return c->active && (int32_t)(c->deadline-now)>0 ? c->deadline-now : 0; }
void ble_npl_callout_set_arg(struct ble_npl_callout *c,void *arg) { c->ev.arg=arg; }
ble_npl_time_t ble_npl_time_get(void) { return ble_port_ms(); }
ble_npl_error_t ble_npl_time_ms_to_ticks(uint32_t ms,ble_npl_time_t *out) { *out=ms; return 0; }
ble_npl_error_t ble_npl_time_ticks_to_ms(ble_npl_time_t ticks,uint32_t *out) { *out=ticks; return 0; }
ble_npl_time_t ble_npl_time_ms_to_ticks32(uint32_t ms) { return ms; }
uint32_t ble_npl_time_ticks_to_ms32(ble_npl_time_t ticks) { return ticks; }
void ble_npl_time_delay(ble_npl_time_t ticks) { uint32_t start=ble_port_ms(); while((uint32_t)(ble_port_ms()-start)<ticks) ble_host_wait_step(); }
uint32_t ble_npl_hw_enter_critical(void) { uint32_t key=ble_port_lock(); critical_depth++; return key; }
void ble_npl_hw_exit_critical(uint32_t key) { if(critical_depth) critical_depth--; ble_port_unlock(key); }
bool ble_npl_hw_is_in_critical(void) { return critical_depth!=0; }
void ble_npl_poll(void)
{
    struct ble_npl_callout *c;
    uint32_t now=ble_port_ms();
    for(c=callouts;c;c=c->next) if(c->active && (int32_t)(now-c->deadline)>=0) {
        c->active=false;
        if(c->q) ble_npl_eventq_put(c->q,&c->ev); else ble_npl_event_run(&c->ev);
    }
}
