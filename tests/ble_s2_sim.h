/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef BLE_S2_SIM_H
#define BLE_S2_SIM_H
extern int sim_executor_enabled;
extern void (*sim_lock_hook)(void);
#include "../open_ble/ble_link.h"
extern uint32_t sim_ticks,sim_starts,sim_stops,sim_windows,sim_applies;
extern ble_conn_t sim_applied;
extern uint8_t sim_apply_kind;
extern uint16_t sim_apply_event,sim_registered_instant;
extern uint32_t sim_apply_ticks;
extern int sim_stop_result,sim_slot[2],sim_latency_enabled;
extern int sim_start_steps,sim_start_failure;
extern ble_adv_t sim_advertising;
#if FELUCCA_BLE_STAGE >= 3
extern uint8_t sim_air[2][31],sim_air_len[2];
#endif
void sim_reset(void);
void sim_advance_us(uint32_t us);
void sim_ack(unsigned slot);
ble_conn_t sim_connection(void);
#endif
