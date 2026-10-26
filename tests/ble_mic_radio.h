/* SPDX-License-Identifier: MIT OR MPL-2.0 */
#pragma once
#include "ble_s3_sim.h"
void sim_radio_start(void);
int sim_radio_receive(uint16_t status,uint16_t header,const uint8_t *data,uint16_t event);
void sim_radio_service(int recovery);
unsigned sim_radio_full(void);
extern void (*sim_radio_release_hook)(void);
extern uint32_t sim_radio_pressure,sim_radio_both,sim_radio_refused;
/* Core ARQ peripheral baseband model. RX service is controlled separately. */
uint8_t sim_core_start(uint8_t rx_sn);
int sim_core_receive(uint16_t header,const uint8_t *data,uint16_t event,int crc);
void sim_core_transmit(uint16_t *header,uint8_t data[31]);
extern uint32_t sim_core_empty_pending_loads;
