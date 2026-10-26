/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* Common clean-room WL82 S1/S2 API. Hardware facts: frozen specifications. */
#ifndef FELUCCA_BLE_S1_H
#define FELUCCA_BLE_S1_H
#include <stdint.h>
#include <stddef.h>
#include "ble_diag.h"

#define BLE_FRAME_SIZE 324u
#define BLE_BB_SIZE 3352u
#define BLE_TRACE_N 512u
#define BLE_HCI_MAX 254u
#ifndef FELUCCA_BLE_STAGE
#define FELUCCA_BLE_STAGE 2
#endif
#ifndef FELUCCA_BLE_S31
#define FELUCCA_BLE_S31 0
#endif
#if FELUCCA_BLE_S31 && FELUCCA_BLE_STAGE < 3
#error "S3.1 requires stage 3"
#endif
#ifndef FELUCCA_BLE_TX_DBM
#define FELUCCA_BLE_TX_DBM 127 /* unknown; command 2007 is then unsupported */
#endif

enum {
    BF_ADV_CFG=4, BF_MODE=6, BF_OPTIONS=8, BF_TX_TOGGLE=14, BF_RX_TOGGLE=16,
    BF_TX_PTR=18, BF_TX_ADV=22, BF_TX_DATA=26, BF_RX_PTR=30, BF_WIN=34,
    BF_RX_ADV=38, BF_RX_DATA=42, BF_CHMAP=46, BF_RX_MAX=54, BF_RX_STAT=56,
    BF_FILTER=60, BF_CRC=68, BF_PEER=76, BF_LOCAL=82, BF_EVT=88,
    BF_PRIO_STAT=94, BF_PRIO_CTRL=96, BF_FLOW=98, BF_TX_CTL=100, BF_RX_CTL=102,
    BF_FREQ_IDX=104, BF_FREQ_USED=144, BF_FREQ_TBL=184, BF_FREQ_USED_TBL=224,
    BF_WIN_STEADY=274, BF_POWER=288, BF_TX_SET=296
};

/* r3 02 §3.3: this aggregate is two halfwords at 0x22 and 0x24.
 * A word access at 0x22 is unaligned on pi32v2. Keep volatile halfword stores
 * even when the value fits in the low half (first/steady/missed windows). */
static inline void ble_frame_window_set(volatile uint16_t *frame,uint32_t us)
{
    frame[BF_WIN/2]=(uint16_t)us;
    frame[BF_WIN/2+1]=(uint16_t)(us>>16);
}

typedef struct {
    uint8_t xtal[2], pa[7], mcs[20], trim[68], public_addr[6];
    uint8_t present; /* bits 0..4: valid 106,107,108,187,102 (outer record CRC) */
    uint8_t vm_status, btif_status;
} ble_cal_t;

typedef struct {
    uint16_t min, max;
    uint8_t type, own_type, peer_type, peer[6], channels, filter;
} ble_adv_params_t;

typedef struct {
    ble_adv_params_t params;
    uint8_t addr[6], data[31], scan[31], data_len, scan_len, power;
} ble_adv_t;

typedef struct {
    uint32_t loop_iters_per_us_x1000, loop_ticks, irq_ticks_while_masked, p33_timeouts;
    uint32_t bringup_ticks, bringup_end_ticks, error, stage, failure_stage, calibration;
    uint32_t event_irqs, rx_irqs, adv_events, scan_req, connect_ignored;
    uint32_t bad_rx, rx_max_ticks, event_max_ticks, irq_over_budget;
    uint32_t event_work_max_ticks, event_over_150us, critical_max_ticks, critical_over_390us;
    uint32_t worker_gap_ticks, stop_timeouts, restarts, enable_ticks, first_event_ticks;
    uint32_t worker_timer_calls, worker_timer_last_ticks, worker_timer_gap_ticks, worker_timer_over_gap;
    uint32_t executor_selftest,executor_dispatch_ticks,executor_pass_ticks,executor_over_budget,executor_starvation;
    uint32_t refill_max_ticks,refill_over_60us;
    uint32_t dma_base, dma_end, modem_base, canary_errors;
#if FM1B_BLE_DIAG
    uint32_t clocks[4],irq_probe[4],step_ticks[16];
    uint8_t p33_before[38], p33_after[38], vco[48];
#endif
} ble_hw_diag_t;

typedef struct {
    uint32_t ms;
    uint16_t len;
    uint8_t dir_type, intake;
    uint8_t data[32];
} ble_trace_t;

typedef struct {
#if FM1B_BLE_DIAG
    uint32_t commands,replies,trace_count,trace_frozen_drops,first_command_ticks;
#endif
    uint32_t rejected,overflow;
    uint8_t enabled, stopping, driver_step, driver_error, last_status;
    uint16_t last_opcode;
} ble_diag_t;

typedef int (*ble_flash_read_fn)(uint32_t off, uint8_t *dst, uint32_t n);
typedef void (*ble_host_fn)(uint8_t type, const uint8_t *packet, uint16_t n);

uint16_t ble_get16(const uint8_t *p);
uint32_t ble_get32(const uint8_t *p);
void ble_put16(uint8_t *p, uint16_t n);
void ble_put32(uint8_t *p, uint32_t n);
uint16_t ble_crc16(uint16_t seed, const uint8_t *p, uint32_t n);
int ble_trim_valid(const uint8_t *p);
int ble_public_valid(const uint8_t *p);
int ble_random_valid(const uint8_t *p);
void ble_derive_public(const uint8_t *mac, uint8_t *addr);
void ble_read_cal(ble_flash_read_fn read, ble_cal_t *cal);
uint8_t ble_adv_validate(const uint8_t *p, ble_adv_params_t *out);
uint8_t ble_frequency(uint8_t ch);
uint16_t ble_adv_channel_word(uint8_t map);
void ble_frame_init(uint8_t *f, const ble_adv_t *adv);
uint16_t ble_adv_header(uint8_t type, uint8_t own_type);
uint8_t ble_adv_payload(uint8_t *out, const uint8_t *addr, const uint8_t *data, uint8_t n);

/* Platform callbacks: controller/main contexts, never host callbacks from ISR.
 * stop_poll: 0 pending, 1 stopped, -1 quarantined.
 * start: 0 published, positive pending (call again), negative failed. While
 * pending the DMA instance remains closed; no subsequent command executes. */
uint32_t ble_port_ticks(void);
uint32_t ble_port_ms(void);
int ble_port_start(const ble_adv_t *adv);
void ble_port_update(const ble_adv_t *adv);
void ble_port_stop_begin(void);
int ble_port_stop_poll(void);
void ble_port_reset_stats(void);
int ble_port_failed(void);
void ble_port_pend(void);

/* Host takes copies during callback; type passed separately, no H4 octet in packet. */
void ble_init(const uint8_t *public_addr, ble_host_fn host);
void ble_set_host(ble_host_fn host);
int ble_hci_send(uint8_t type, const uint8_t *packet, uint16_t n);
void ble_worker(void);
void ble_driver_start(uint8_t type, const uint8_t *random_addr);
void ble_driver_stop(void);
void ble_get_diag(ble_diag_t *out);
void ble_trace_freeze(int freeze);
const ble_trace_t *ble_trace_record(uint32_t oldest_index);
uint32_t ble_trace_size(void);
void ble_set_power(uint8_t level);
#endif
