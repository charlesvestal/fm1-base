/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FELUCCA_BLE_LINK_H
#define FELUCCA_BLE_LINK_H
#include "ble_s1.h"
#define BLE_RX_BACKLOG 19u
#define BLE_RX_STREAM (BLE_RX_BACKLOG+2u) /* boundary markers do not consume PDU capacity */
#define BLE_TX_COUNT (FELUCCA_BLE_STAGE>=3 ? 15u : 13u)
#define BLE_LL_PAYLOAD_MAX (FELUCCA_BLE_STAGE>=3 ? 31u : 27u)
#define BLE_HANDLE 0x50u
#define BLE_TICKS_US 24u
#define BLE_INSTANT_GUARD_US 1000u
#define BLE_RX_IRQ_LAG_US 0u /* A1/A7: retune with a common-clock sniffer. */
#define BLE_FIRST_WINDOW_EXTRA_US 1250u
#define BLE_UPDATE_WINDOW_EXTRA_US 625u
#define BLE_LOCAL_SCA_PPM 500u
#ifndef BLE_INVALID_CONNECT_RESTART
#define BLE_INVALID_CONNECT_RESTART 0 /* A6: enable only if continuation fails on air. */
#endif
#define BLE_WINDOW_PAD_US 50u
#define BLE_MISS_STEP_US 100u
#define BLE_MISS_LIMIT_US 2000u
#define BLE_UPDATE_WIDEN_US 2500u

typedef struct {
    uint16_t interval,latency,timeout,offset;
    uint8_t win,sca,hop,map[5],peer[6],peer_type;
    uint32_t aa,crc;
} ble_conn_t;
typedef struct {
    uint32_t sup,first,est_end,cutoff,inst_reset,win_reset;
    uint16_t timeout,new_timeout;
    uint8_t established,win_pending;
} ble_timeline_t;
enum {
    BLE_IGNORE_NONE, BLE_IGNORE_LENGTH, BLE_IGNORE_PARAMS, BLE_IGNORE_MAP,
    BLE_IGNORE_PENDING, BLE_IGNORE_ROLE, BLE_IGNORE_UNEXPECTED,
    BLE_IGNORE_VERSION, BLE_IGNORE_RESERVED, BLE_IGNORE_ENCRYPTED,
    BLE_IGNORE_COUNT
};
enum {
    BLE_UPDATE_EMPTY, BLE_UPDATE_PENDING, BLE_UPDATE_PROGRAMMED, BLE_UPDATE_APPLIED,
    BLE_UPDATE_INVALID, BLE_UPDATE_BUSY, BLE_UPDATE_LATE, BLE_UPDATE_CLOSED
};
typedef struct {
    uint32_t serial,epoch;
    uint16_t old_interval,new_interval,offset,instant,received;
    uint8_t win,outcome,reason;
} ble_update_diag_t;
typedef struct {
    uint32_t epoch,created,invalid,events,rx_backpressure;
#if FM1B_BLE_DIAG
    uint32_t accepted,repetitions,acl_in,acl_out,acknowledged,ncp,flushed,ignored,rx_delayed,worker_calls,epoch_markers;
#endif
    uint32_t acl_rejected,ctrl_drop;
    uint32_t establishment_fail,supervision_fail,disconnects,updates,map_updates,late_instants;
    uint32_t worker_gap_ticks,worker_max_ticks,worker_over_gap,terminate_timeouts;
#if FM1B_BLE_DIAG
    uint32_t rx_class[4];
#endif
    uint16_t interval,latency,timeout,event,rx_used,tx_used,rx_peak;
    uint8_t state,established,reason,peer_features[8];
#if FM1B_BLE_DIAG
    uint32_t ignore_reasons[BLE_IGNORE_COUNT],update_serial;
    ble_update_diag_t update[4];
#endif
    ble_conn_t pending;
    uint16_t instant,received,ignored_event;
    uint8_t instant_kind,programmed,ignored_opcode,ignored_reason;
} ble_link_diag_t;
/* Single core: all functions except the pure helpers need the port's short
 * nesting-safe critical section. No function here calls a host. */
extern struct ble_link {
    /* Everything after epoch is connection-local; create clears it in full. */
    ble_link_diag_t diag;
    uint32_t epoch;
    ble_conn_t conn,pending;
    ble_timeline_t time;
    uint32_t last_worker,anchor,feature_deadline,terminate_deadline,instant_time,created_at,pending_serial;
    uint16_t anchor_event,closed_n,instant,instant_received,widen,window,acc;
    uint8_t state,closed_valid,rx_seen,valid_seen,first_switch,hold,priority,invalid_events,anchor_seen;
    uint8_t instant_kind,instant_programmed,instant_timed,peer_stop,reason,local_terminate;
    uint8_t created_pending,disconn_pending,feature_pending,features_known,version_sent;
    uint8_t features[8],features_ready,features_status,update_pending,peer_version[5],version_known;
    uint16_t completed;
#if FELUCCA_BLE_S31
    uint8_t peer_identity[6],peer_identity_type,peer_resolved,resolve_index,resolve_done;
#endif
} ble_link;
enum { BLE_LINK_IDLE, BLE_LINK_CREATED, BLE_LINK_UP, BLE_LINK_CLOSING, BLE_LINK_STOPPED };
uint8_t ble_map_count(const uint8_t map[5]);
uint8_t ble_csa1(uint8_t *unmapped,uint8_t hop,const uint8_t map[5]);
uint8_t ble_rx_class(uint16_t status,uint8_t n);
int ble_conn_params_valid(const ble_conn_t *c);
int ble_connect_decode(ble_conn_t *c,const uint8_t *p,uint8_t n,uint8_t header);
uint16_t ble_widen(const ble_conn_t *c);
int ble_instant_valid(uint16_t instant,uint16_t closed);
void ble_timeline_create(ble_timeline_t *t,const ble_conn_t *c,uint32_t now);
uint8_t ble_timeline_rx(ble_timeline_t *t,const ble_conn_t *c,uint8_t cls,uint32_t now);
uint8_t ble_timeline_check(ble_timeline_t *t,const ble_conn_t *c,uint32_t now);
void ble_timeline_arm(ble_timeline_t *t,uint32_t instant,uint32_t window,uint16_t timeout);
uint8_t ble_timeline_rx_delayed(ble_timeline_t *t,uint8_t cls,uint32_t stamp,uint32_t service,int delayed);
int ble_missed_step(int fine,uint32_t raw,unsigned phase,int *next,unsigned *drift);
void ble_ll_init(void);
void ble_ll_reset(void);
/* Reset owns the stopped instance until all software slices finish. */
void ble_ll_reset_begin(void);
int ble_ll_reset_step(void);
int ble_ll_create(const ble_conn_t *c,uint32_t now);
int ble_ll_rx_room(void);
int ble_ll_rx_marker(uint8_t marker);
int ble_ll_rx_delayed(uint16_t status,uint16_t header,const uint8_t *data,uint32_t stamp,uint16_t event,int delayed);
int ble_ll_rx_slot(uint16_t status,uint16_t header,const uint8_t *data,uint32_t stamp,uint16_t event,
                   int delayed,uint8_t slot,uint8_t flags);
int ble_ll_rx(uint16_t status,uint16_t header,const uint8_t *data,uint32_t stamp,uint16_t event);
void ble_ll_event(uint16_t event,uint32_t now);
void ble_ll_worker(void);
int ble_ll_acl(const uint8_t *p,uint16_t n);
int ble_ll_tx_pick(void);
const uint8_t *ble_ll_tx_data(unsigned id);
uint8_t ble_ll_tx_len(unsigned id);
uint8_t ble_ll_tx_llid(unsigned id);
int ble_ll_acl_waiting(void);
void ble_ll_tx_ack(unsigned id);
void ble_ll_close(uint8_t reason);
uint8_t ble_ll_disconnect(uint16_t handle,uint8_t reason);
uint8_t ble_ll_features(uint16_t handle);
int ble_ll_poll(uint8_t *type,uint8_t *p,uint16_t *n);
void ble_ll_get_diag(ble_link_diag_t *d);
int ble_ll_pending(void);
/* HAL implemented in the Felucca adapter; tests use a simulated radio. */
uint32_t ble_port_lock(void);
void ble_port_unlock(uint32_t key);
void ble_port_refill(void);
void ble_port_refill_one(void); /* executor item: at most one CCM/load */
void ble_port_rx_recover(void);
void ble_port_window(uint32_t us);
void ble_port_instant(uint8_t kind,uint16_t instant,const ble_conn_t *c);
void ble_port_apply(uint8_t kind,const ble_conn_t *c,uint16_t widening,uint32_t window);
void ble_port_latency(uint16_t latency,int enabled);
void ble_port_priority(unsigned priority);
#endif
