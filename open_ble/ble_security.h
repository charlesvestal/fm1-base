/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FELUCCA_BLE_SECURITY_H
#define FELUCCA_BLE_SECURITY_H
#include <stdint.h>
#include "ble_diag.h"
#include "ble_reliability.h"
enum { BLE_ENC_N, BLE_ENC_S1, BLE_ENC_S2, BLE_ENC_PA, BLE_ENC_PB, BLE_ENC_PC };
enum { BLE_MARK_RX_ON=1, BLE_MARK_RX_OFF, BLE_MARK_REJECT_ACK };
typedef struct {
    uint64_t tx_counter,rx_counter;
    uint32_t mic_failures,phase_errors,starts,refreshes,ltk_requests,negative_replies;
    uint8_t phase,tx_on,rx_on,encrypted;
} ble_security_diag_t;
void ble_security_get_diag(ble_security_diag_t *out);
enum { BLE_MIC_RECORDS=4, BLE_RX_DELAYED=1, BLE_RX_REPEAT=2, BLE_RX_PRESSURE=4,
       BLE_RX_EVENT_UNKNOWN=8, BLE_RX_BOTH_FULL=16, BLE_RX_RECOVERY=32 };
#if FM1B_BLE_DIAG
typedef struct {
    uint64_t counter;
    uint8_t sn,length;
} ble_mic_pdu_t;
typedef struct {
    uint32_t serial,ms,epoch,received_ms,since_update_ms;
    uint64_t counter;
    uint16_t event,service_event,interval,header,status;
    uint8_t slot,flags,rx_count,tx_count;
    ble_mic_pdu_t rx[4],tx[2]; /* oldest first; metadata only */
} ble_mic_record_t;
unsigned ble_security_mic_records(ble_mic_record_t out[BLE_MIC_RECORDS]);
void ble_security_tx_loaded(unsigned id,uint8_t sn);
/* R6 descriptor observations; no claim of an on-air TX sniffer. */
enum { BLE_RADIO_EVENTS=32, BLE_RADIO_ROWS=64 };
enum { BLE_RADIO_RX=1, BLE_RADIO_LOAD, BLE_RADIO_ACK, BLE_RADIO_TX_STATE, BLE_RADIO_PREP };
enum { BLE_RADIO_ACCEPT=1, BLE_RADIO_DUPLICATE, BLE_RADIO_CRC, BLE_RADIO_OTHER,
       BLE_RADIO_AUTH_OK, BLE_RADIO_AUTH_BAD };
#define BLE_RADIO_NO_COUNTER UINT64_MAX
typedef struct {
    uint64_t counter;
    uint32_t ticks;
    uint16_t header,status;
    uint8_t kind,slot,id,decision,flags;
} ble_radio_row_t;
typedef struct {
    uint32_t serial,epoch;
    uint16_t event,count,overflow;
    uint8_t closed;
    ble_radio_row_t row[BLE_RADIO_ROWS];
} ble_radio_event_t;
typedef struct { uint32_t first,last; uint8_t frozen,reason; } ble_radio_summary_t;
void ble_radio_event(uint16_t event,int closed);
uint64_t ble_radio_rx(uint16_t event,uint16_t status,uint16_t header,uint8_t slot,uint8_t flags);
void ble_radio_tx(uint8_t kind,uint16_t event,unsigned slot,int id,uint16_t header,uint16_t status);
void ble_radio_freeze(uint8_t reason);
void ble_radio_summary(ble_radio_summary_t *out);
int ble_radio_read(uint32_t serial,ble_radio_event_t *out);
#else
#define ble_security_tx_loaded(id,sn) ((void)(id),(void)(sn))
#define ble_radio_event(event,closed) ((void)0)
#define ble_radio_rx(event,status,header,slot,flags) 0u
#define ble_radio_tx(kind,event,slot,id,header,status) ((void)0)
#define ble_radio_freeze(reason) ((void)0)
#endif
uint8_t ble_ll_ltk_reply(uint16_t handle,const uint8_t *ltk,int negative);
int ble_ll_tx_prepare(unsigned id,uint8_t out[31],uint8_t *n);
uint8_t ble_ll_tx_boundary(unsigned id);
int ble_ll_rx_reserve(unsigned pdus,unsigned markers);
int ble_ll_rx_after_marker(uint8_t marker,uint16_t status,uint16_t header,const uint8_t *data);
#ifdef BLE_HOST_TEST
void ble_security_test_counters(uint64_t tx,uint64_t rx);
#endif
#endif
