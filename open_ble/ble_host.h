/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FELUCCA_BLE_HOST_H
#define FELUCCA_BLE_HOST_H
#include <stdint.h>
#include "ble_diag.h"
#include "ble_midi.h"
#define BLE_HOST_EVENT_COUNT 32u
enum ble_host_event_kind {
    BLE_HE_CONNECT=1,BLE_HE_DISCONNECT,BLE_HE_ENC_START,BLE_HE_ENC_REFRESH,
    BLE_HE_SMP_START,BLE_HE_SMP_RESULT,BLE_HE_MTU,BLE_HE_CCCD_WRITE,
    BLE_HE_CCCD_RESTORE,BLE_HE_SC_SENT,BLE_HE_SC_CONFIRMED,
    BLE_HE_PARAM_SENT,BLE_HE_PARAM_RESULT,BLE_HE_CONN_UPDATE,BLE_HE_MIDI_READ,
    BLE_HE_ATT_REJECT
};
typedef struct {
    uint32_t ms,session,a,b,c;
    uint16_t connection,kind;
} ble_host_event_t;
#if FM1B_BLE_DIAG
/* Main context only, including the pinned host's diagnostic hooks. No keys. */
void ble_host_event(unsigned kind,uint16_t connection,uint32_t a,uint32_t b,uint32_t c);
unsigned ble_host_events(ble_host_event_t out[BLE_HOST_EVENT_COUNT]);
#else
static inline void ble_host_event(unsigned kind,uint16_t connection,uint32_t a,uint32_t b,uint32_t c)
{ (void)kind; (void)connection; (void)a; (void)b; (void)c; }
#endif
typedef struct {
    uint32_t syncs,resets,error,event_drop,acl_drop,midi_in,midi_out,midi_bad,midi_drop;
    uint32_t param_requests,param_status,gatt_invalidations,gatt_confirmations,gatt_peer_version;
    uint32_t main_gap_ms,heap_used,heap_peak,heap_fail,assertions;
    uint16_t connection,mtu; uint8_t synced,subscribed,running,echo,param_pending;
#if FELUCCA_BLE_STAGE >= 3
    uint32_t security_requests,security_skipped,security_already,pair_failures,pair_status;
    uint8_t encrypted,bonded,key_size,pair_method,sc,identity_known,peer_identity_type,peer_identity[6];
#endif
} ble_host_diag_t;
void ble_host_start(void);
void ble_host_enable(int on);
void ble_host_off_reason(uint8_t reason);
int ble_host_pause_advertising(void);
void ble_host_resume_advertising(void);
uint32_t ble_host_midi_room(void);
int ble_host_linked(void);
int ble_host_connected(void);
void ble_host_worker(void);
void ble_host_clock_read(ble_midi_clock_diag_t *out);
void ble_host_manual(void);
void ble_host_resume(void);
void ble_host_echo(int enabled);
void ble_host_get_diag(ble_host_diag_t *d);
void ble_host_midi_out(uint32_t usb_packet,uint32_t ms);
int ble_host_midi_take(uint32_t *usb_packet,uint32_t *ms);
void ble_host_wait_step(void);
void ble_host_assert(const char *file,int line);
#if FELUCCA_BLE_STAGE >= 3
/* Application unpair/replacement: remove privacy entry and records, keep link. */
int ble_host_delete_bond(uint8_t identity_type,const uint8_t identity[6]);
#endif
#if FELUCCA_BLE_S31
void ble_host_local_irk(const uint8_t address[6],uint8_t irk[16]);
#endif
#endif
