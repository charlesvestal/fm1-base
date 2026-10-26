/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
/* The open BLE build's interrupt entries: felucca-bsp's chip/fm1_isr.S emits these in place of its two defaults
 * (tools/build.py: -DFM1_ISR_ENTRIES="ble_open_isr.inc"). Every handler above the TIMER0 link-layer executor (level 2)
 * that returns, TIMER5 (tick, panel scan, USB/TRS: 4), audio (3) and baseband RX / event (5), is wrapped with the
 * nested preemption accounting (open_ble/ble_timing.c), so the executor subtracts their time once; the executor's own
 * entry is not accounted. The BLE vectors and their priorities: open_ble/hal/fm1_ble.h (fm1_irq_attach). */
	FM1_ISR isr_timer5, fm1_timer5_irq, ble_time_preempt_enter, ble_time_preempt_leave
	FM1_ISR isr_alnk0, fm1_alnk0_irq, ble_time_preempt_enter, ble_time_preempt_leave
	FM1_ISR isr_ble_rx, fm1_ble_rx_irq, ble_time_preempt_enter, ble_time_preempt_leave
	FM1_ISR isr_ble_event, fm1_ble_event_irq, ble_time_preempt_enter, ble_time_preempt_leave
	FM1_ISR isr_ble_worker, fm1_ble_worker_irq
