/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David */
/* felucca-bsp's chip/fm1_wifi.h on the host: the same calls and table places, with the radio a stand-in. A frame sent
 * goes to fm1_fake_tx (the host's simulated network, host/wifi_air.c, or a test's log), a frame received comes from
 * fm1_fake_rx (laid out as the RX DMA leaves it: a big-endian length word, the 16-byte RXWI, the 802.11 frame), and the
 * MAC's registers are written to fm1_fake_mac_wr (a test's record). Nothing is encrypted: the MAC's CCMP is the
 * hardware's, so a Protected frame goes by in the clear, as the MAC hands it over once decrypted. */
#pragma once
#include <stdint.h>
#include <string.h>

#define FM1_WIFI_TX_HDR 24u
#define FM1_WIFI_RXWI 16u
#define FM1_WIFI_RX_MAX 4088u
#define FM1_WIFI_DMA_TOP 0x01C20000u
#define FM1_WIFI_ADDR_OWN 0x1008u
#define FM1_WIFI_ADDR_BSSID 0x1010u
#define FM1_WIFI_RX_FILTER 0x1400u
#define FM1_WIFI_RX_FILTER_ALL 0x1u
#define FM1_WIFI_WCID_ADDR(w) (0x1800u + 8u * (w))
#define FM1_WIFI_PKEY(w) (0x4000u + 32u * (w))
#define FM1_WIFI_IVEIV(w) (0x6000u + 8u * (w))
#define FM1_WIFI_WCID_ATTR(w) (0x6800u + 4u * (w))
#define FM1_WIFI_SKEY(i) (0x6C00u + 32u * (i))
#define FM1_WIFI_SKEY_MODE 0x7000u
#define FM1_WIFI_CIPHER_CCMP 4u

static void (*fm1_fake_mac_wr)(uint32_t r, uint32_t v);
static int (*fm1_fake_tx)(const uint8_t *frame, uint32_t len, uint32_t wcid, uint32_t ack);
static int32_t (*fm1_fake_rx)(uint32_t *buf, uint32_t max);
static uint32_t fm1_fake_ch = 1, fm1_fake_up;
static void wifi_sta_set_test_mac_wr(void (*fn)(uint32_t r, uint32_t v)) { fm1_fake_mac_wr = fn; }   /* (a test's) */

#define FM1_WIFI_RX_FILTER_STA 0x17F97u
#define FM1_WIFI_TXWI(phy, mcs) (0xB0u | ((uint32_t)(mcs) & 0x7Fu) << 16 | ((uint32_t)(phy) & 3u) << 30)
#define FM1_WIFI_RATE_CCK1 FM1_WIFI_TXWI(0, 0)
#define FM1_WIFI_RATE_CCK2 FM1_WIFI_TXWI(0, 1)
#define FM1_WIFI_RATE_OFDM6 FM1_WIFI_TXWI(1, 0)
#define FM1_WIFI_LG_FBK0 0x135Cu
#define FM1_WIFI_LG_FBK1 0x1360u
#define FM1_WIFI_LG_FBK0_SDK 0xEDCBA988u
#define FM1_WIFI_LG_FBK1_SDK 0x00002100u
#define FM1_WIFI_AGC_WIN 0u
#define FM1_WIFI_AGC_ENTRY 44u
#define FM1_WIFI_AGC_START 66u
static inline void fm1_wifi_tx_prepare_rate(uint32_t *buf, uint32_t mpdu_len, uint32_t wcid, uint32_t ack, uint32_t w0)
{
    buf[0] = mpdu_len + 22u;
    buf[1] = 4u;
    buf[2] = w0;
    buf[3] = (ack & 1u) | 13u << 2 | (wcid & 0xFFu) << 8 | (mpdu_len & 0xFFFu) << 16;
    buf[4] = buf[5] = 0;
}
static inline void fm1_wifi_tx_prepare(uint32_t *buf, uint32_t mpdu_len, uint32_t wcid, uint32_t ack)
{
    fm1_wifi_tx_prepare_rate(buf, mpdu_len, wcid, ack, FM1_WIFI_RATE_CCK1);
}
/* the MAC's frame counters as Ralink's (clear when read): every frame sent with ACK counts once, as acknowledged at
 * once, after a retry (fm1_fake_retry_pct of them) or never (fm1_fake_lost_pct: the frame is not delivered); the
 * receive queue's overflows (fm1_fake_rx_over, the simulated network's queue full). A frame sent at another rate than
 * 1 Mb/s is lost by fm1_fake_lost_fast_pct more (a link that holds at 1 Mb/s and not faster); fm1_fake_w0 the last
 * frame's TXWI word 0, fm1_fake_n_fast the frames sent faster */
static uint32_t fm1_fake_ok, fm1_fake_rty, fm1_fake_fail, fm1_fake_rx_over, fm1_fake_retry_pct, fm1_fake_lost_pct, fm1_fake_n;
static uint32_t fm1_fake_lost_fast_pct, fm1_fake_w0 = FM1_WIFI_RATE_CCK1, fm1_fake_n_fast;
static inline int fm1_wifi_tx(uint32_t *buf)
{
    uint32_t len = buf[3] >> 16 & 0xFFFu, k, lost = fm1_fake_lost_pct + (buf[2] == FM1_WIFI_RATE_CCK1 ? 0u : fm1_fake_lost_fast_pct);
    fm1_fake_w0 = buf[2];
    fm1_fake_n_fast += buf[2] != FM1_WIFI_RATE_CCK1;
    if (buf[3] & 1u) {                            /* (an even spread over each 100 frames: n * pct crosses a whole) */
        k = fm1_fake_n++ % 100u;
        if ((k + 1u) * lost / 100u != k * lost / 100u) {
            fm1_fake_fail++;
            return 0;                             /* (sent; the retries used up, nobody heard it) */
        }
        if ((k + 1u) * fm1_fake_retry_pct / 100u != k * fm1_fake_retry_pct / 100u)
            fm1_fake_rty++;
        else
            fm1_fake_ok++;
    } else {
        fm1_fake_ok++;
    }
    return fm1_fake_tx ? fm1_fake_tx((const uint8_t *)buf + FM1_WIFI_TX_HDR, len, buf[3] >> 8 & 0xFFu, buf[3] & 1u) : 0;
}
static inline uint32_t fm1_wifi_rx_mpdu_len(const uint8_t *f) { return (uint32_t)(f[2] | f[3] << 8) & 0xFFFu; }
static inline int32_t fm1_wifi_rx_rssi(const uint8_t *f) { return (int8_t)f[8]; }
static inline uint32_t fm1_fake_take(uint32_t *c)
{
    uint32_t v = *c > 0xFFFFu ? 0xFFFFu : *c;
    *c = 0;
    return v;
}
static inline uint32_t fm1_wifi_mac_rd(uint32_t r)
{
    if (r == 0x1708u)                             /* RX_STA_CNT2 [31:16] */
        return fm1_fake_take(&fm1_fake_rx_over) << 16;
    if (r == 0x170Cu)                             /* TX_STA_CNT0 [15:0] */
        return fm1_fake_take(&fm1_fake_fail);
    if (r == 0x1710u) {                           /* TX_STA_CNT1 [15:0] at once, [31:16] after a retry */
        uint32_t ok = fm1_fake_take(&fm1_fake_ok);
        return ok | fm1_fake_take(&fm1_fake_rty) << 16;
    }
    return 0;
}
static inline void fm1_wifi_mac_wr(uint32_t r, uint32_t v)
{
    if (fm1_fake_mac_wr)
        fm1_fake_mac_wr(r, v);
}
static inline void fm1_wifi_key_wr(uint32_t r, const uint8_t *key)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        fm1_wifi_mac_wr(r + 4u * i, key && i < 4u ? (uint32_t)key[4 * i] | (uint32_t)key[4 * i + 1] << 8 |
                                                         (uint32_t)key[4 * i + 2] << 16 | (uint32_t)key[4 * i + 3] << 24 : 0u);
}
static inline void fm1_wifi_addr_wr(uint32_t r, const uint8_t *mac, uint32_t hi_or)
{
    fm1_wifi_mac_wr(r, (uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24);
    fm1_wifi_mac_wr(r + 4u, (uint32_t)mac[4] | (uint32_t)mac[5] << 8 | hi_or);
}
static inline uint32_t fm1_wifi_channel(void) { return fm1_fake_ch; }
static inline void fm1_wifi_set_channel(uint32_t ch) { fm1_fake_ch = ch; }
static inline void fm1_wifi_up(const uint16_t *hi, const uint8_t *code, uint32_t n)
{
    (void)hi, (void)code, (void)n;
    fm1_fake_up = 1;
}
static inline int fm1_wifi_is_up(void) { return fm1_fake_up != 0u; }
static inline int32_t fm1_wifi_rx(uint32_t *buf, uint32_t max) { return fm1_fake_rx ? fm1_fake_rx(buf, max) : 0; }

/* the radio's tuning side (felucca-bsp's fm1_ble_rf.h, fm1_wifi.h; device/wifi_own_tune.c): the BBP window writes go to
 * fm1_fake_bbp_wr (a test's record), the Wi-Fi IQ rewrite is logged as the record it was given (fm1_fake_cal_last: 0 the
 * start's values back), the flash reads come from fm1_fake_flash (none: every read fails, no VM) */
static void (*fm1_fake_bbp_wr)(uint32_t w, uint32_t e, uint32_t v);
static int (*fm1_fake_flash)(uint32_t off, uint8_t *dst, uint32_t n);
static uint32_t fm1_fake_agc = FM1_WIFI_AGC_START, fm1_fake_cal_calls;
static const uint8_t *fm1_fake_cal_last;
static inline int fm1_fake_flash_rd(uint32_t off, uint8_t *dst, uint32_t n)
{
    return fm1_fake_flash ? fm1_fake_flash(off, dst, n) : -1;
}
static inline void fm1_bbp_win_wr(uint32_t w, uint32_t e, uint32_t v)
{
    if (w == FM1_WIFI_AGC_WIN && e == FM1_WIFI_AGC_ENTRY)
        fm1_fake_agc = v;
    if (fm1_fake_bbp_wr)
        fm1_fake_bbp_wr(w, e, v);
}
static inline uint32_t fm1_rf_cal_wifi_iq(const uint8_t *x187)
{
    fm1_fake_cal_calls++;
    fm1_fake_cal_last = x187;
    return 12u;
}
static inline void fm1_wifi_fallback_sdk(void)
{
    fm1_wifi_mac_wr(FM1_WIFI_LG_FBK0, FM1_WIFI_LG_FBK0_SDK);
    fm1_wifi_mac_wr(FM1_WIFI_LG_FBK1, (fm1_wifi_mac_rd(FM1_WIFI_LG_FBK1) & ~0xFFFFu) | FM1_WIFI_LG_FBK1_SDK);
}
