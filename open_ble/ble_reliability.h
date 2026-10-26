/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* R7: fm1-base diagnostic build only. This public record contains no keys. */
#pragma once
#include "ble_diag.h"
#include <stdint.h>
#if FM1B_BLE_RELIABILITY
/* Results: 0 fail/mismatch, 1 pass/match, 2 unavailable. */
typedef struct {
    uint32_t con,epoch,last_epoch,operations,blocks,timeouts;
    uint8_t busy;
} ble_aes_state_t;
typedef struct {
    uint32_t epoch,ms,connect_gap_ms,connect_close_gap_ms,previous_disconnect_ms,previous_close_ms,enc_req_age_ms,start_ack_age_ms;
    uint32_t old_work_create,old_work_enc;
    uint64_t counter;
    uint16_t header,status,event,near_valid,near_pass;
    uint8_t kind,complete,slot,length,direction;
    uint8_t hw_retry,sw_same,key_recomputed,key_unchanged,iv_unchanged,previous_key,previous_context,kat;
    uint8_t dma_valid[2],dma_equal,dma_input_equal[2],derived_valid,inputs_retained,previous_valid;
    uint8_t input_stable,raw_before[31],raw[31],dma[2][31];
    uint16_t dma_header[2],dma_status[2];
    uint8_t rand_ediv[10],skd[16],iv[8],iv_used[8];
    ble_aes_state_t before,failed,after_retry,after;
} ble_reliability_record_t;
typedef struct {
    uint32_t kat_runs,kat_failures,tx_checked,tx_mismatches;
    uint32_t steps,max_ticks,over100,max_wall_ticks,tx_hold_max_ticks;
    uint64_t own_ticks;
} ble_reliability_stats_t;
void ble_port_aes_state(ble_aes_state_t *out);
int ble_port_diag_rx_read(unsigned slot,uint16_t *header,uint16_t *status,uint8_t out[31]);
void ble_port_diag_rx_release(unsigned slot);
void ble_reliability_get(ble_reliability_stats_t *stats,ble_reliability_record_t *rx,ble_reliability_record_t *tx);
int ble_reliability_failed(void);
int ble_reliability_pending(void);
int ble_port_diag_radio_idle(void);
uint32_t ble_reliability_old_commands(int all);
#endif
