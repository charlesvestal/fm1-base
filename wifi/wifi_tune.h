/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David (hdavid), Optimist (github.com/w0ts/optimist) */
/* The own Wi-Fi stack's tuning (device/wifi_own.c), four switches the user A/Bs on the FM-1 with SYSTEM > WI-FI's
 * numbers (signal, RETRY, LOST), each after what JieLi's AC79 SDK does and the own stack did not (the review of
 * 2026-10-29, fm1-next-wifirx):
 *   WT_CAL   (a) the FM-1's stored calibration (VM record 187: its TX gains, phases and DCs, its RX gains and phases)
 *            into the Wi-Fi PHY once it is up, when the record's check holds: JieLi's RFIinitUseTrimValue = 1
 *            (wf_rf_init.c wifi_trim_is_already, quick_start_wifi_trim; RF-CALIBRATION.md §4.5), as Optimist's own
 *            firmware takes it. Off, or no valid record: the start's own measurement, which leaves those neutral
 *            (felucca-bsp fm1_rf_cal.h FM1_RF_CAL_FULL 0). The BT side never changes (its trims are set at the start).
 *   WT_FILT  (b) the MAC's receive filter at JieLi's station value once associated (0x1400 = 0x17F97, Ralink's
 *            STANORMAL: felucca-bsp fm1_wifi.h FM1_WIFI_RX_FILTER_STA), everything in (0x1) while scanning, joining and
 *            in MODE AP, as before.
 *   WT_AGC   (c) the receiver's highest AGC gain from the joined AP's signal, JieLi's adj_agc_max_gain steps (wf_soft_adj.c:
 *            34 above -36 dBm, 43, 51, 57, 66 below -76) through window C entry 44, from the mean of the AP's frames'
 *            signal (wifi_own.c's, the one plat_wifi_quality shows), with WT_AGC_HYST dB of hysteresis; 66 (the start's
 *            value) when the AP was not heard for WT_AGC_IDLE_MS, while scanning and in MODE AP.
 *   WT_RATE  (f) unicast data with an ACK at CCK 2 Mb/s once joined (management, EAPOL, group and no-ACK frames stay at
 *            1 Mb/s); the MAC's own retries of such a frame at 1 Mb/s (its fallback table, JieLi's: CCK 2M -> 1M); and
 *            1 Mb/s for everything for a while once WT_RATE_LOST_N frames in a second were never acknowledged.
 * The mask is kept in the Wi-Fi record's spare byte (wifi_nets.h struct wn_store rsv: WT_KEPT | the mask; 0 = the build's
 * default). This file is the pure part (no radio): the record check, the VM walk, the AGC steps, the rate's state. */
#ifndef WIFI_TUNE_H
#define WIFI_TUNE_H
#include <stdint.h>

#define WT_CAL 1u
#define WT_FILT 2u
#define WT_AGC 4u
#define WT_RATE 8u
#define WT_ALL 15u
#define WT_KEPT 0x80u                    /* the record's byte: a mask was chosen (else the build's default) */

uint32_t wt_mask_from(uint8_t kept, uint32_t def);     /* the mask in use from the record's byte */
uint8_t wt_mask_keep(uint32_t mask);                   /* .. and the byte that keeps one */

/* ---- (a) the stored calibration: VM record 187 (68 bytes: 64, then CRC-16/XMODEM of them little-endian, then 00 00) */
#define WT_187_LEN 68u
int wt_187_ok(const uint8_t r[WT_187_LEN]);            /* the record's own check holds (and is not 0, the SDK's "none") */
/* the SDK's VM in flash (stock V15's, docs/BLE-HW-FACTS.md §14 in Optimist; bt/ble/ble_vm.h has the reader of all four
 * records): the magic 55 AA AA 55, then records [check = low byte of the CRC of the data] [id 7:0] [id 11:8 | len 3:0 << 4]
 * [len 11:4] and the data, back to back; the log ends at the first failing check. -> 1 and out = the last valid 187 whose
 * own check holds; 0 none (no VM, no such record, a read failed) */
typedef int (*wt_read_fn)(void *ctx, uint32_t off, uint8_t *dst, uint32_t n);   /* 0 = read */
int wt_vm_187(wt_read_fn rd, void *ctx, uint32_t area, uint32_t size, uint8_t out[WT_187_LEN]);
#define WT_VM_AREA 0x0E8000u             /* stock V15's VM on an FM-1 (measured: bt/ble/ble_vm.h BLE_VM_HW) */
#define WT_VM_SIZE 0x1000u

/* ---- (c) the AGC's highest gain */
#define WT_AGC_HYST 3                    /* dB past a step's edge before the gain moves */
#define WT_AGC_IDLE_MS 220u              /* nothing heard from the AP this long: the start's gain (JieLi: 2 x 110 ms) */
#define WT_AGC_FREE 66u                  /* the start's (felucca-bsp FM1_WIFI_AGC_START) */
uint32_t wt_agc_step(int32_t rssi);                    /* JieLi's step for a signal (dBm), no hysteresis */
uint32_t wt_agc_next(uint32_t gain, int32_t rssi);     /* the gain from the one in use and the signal's mean, with hysteresis */

/* ---- (f) the data rate */
#define WT_RATE_LOST_N 3u                /* frames never acknowledged in a second: 1 Mb/s for a while */
#define WT_RATE_HOLD_S 10u               /* .. this many seconds, doubled (to 80) when it falls again soon after */
#define WT_RATE_SOON_S 30u
struct wt_rate {
    uint8_t slow;                        /* 1: everything at 1 Mb/s now */
    uint8_t k;                           /* the hold's doubling (0..3) */
    uint32_t t_until, t_fast;            /* ms: back to the fast rate at; the fast rate began at */
    uint32_t falls;                      /* times it fell back since the start */
};
void wt_rate_reset(struct wt_rate *r, uint32_t now_ms);
void wt_rate_second(struct wt_rate *r, uint32_t now_ms, uint32_t lost);   /* a second's never-acknowledged frames */
/* the frame (802.11, len bytes) at the fast rate? on: WT_RATE in the mask and joined -> 1 fast, 0 at 1 Mb/s */
int wt_rate_fast(const struct wt_rate *r, int on, const uint8_t *f, uint32_t len, uint32_t ack);
#endif
