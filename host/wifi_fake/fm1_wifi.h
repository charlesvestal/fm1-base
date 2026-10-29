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

static inline void fm1_wifi_tx_prepare(uint32_t *buf, uint32_t mpdu_len, uint32_t wcid, uint32_t ack)
{
    buf[0] = mpdu_len + 22u;
    buf[1] = 4u;
    buf[2] = 0xB0u;
    buf[3] = (ack & 1u) | 13u << 2 | (wcid & 0xFFu) << 8 | (mpdu_len & 0xFFFu) << 16;
    buf[4] = buf[5] = 0;
}
static inline int fm1_wifi_tx(uint32_t *buf)
{
    uint32_t len = buf[3] >> 16 & 0xFFFu;
    return fm1_fake_tx ? fm1_fake_tx((const uint8_t *)buf + FM1_WIFI_TX_HDR, len, buf[3] >> 8 & 0xFFu, buf[3] & 1u) : 0;
}
static inline uint32_t fm1_wifi_rx_mpdu_len(const uint8_t *f) { return (uint32_t)(f[2] | f[3] << 8) & 0xFFFu; }
static inline int32_t fm1_wifi_rx_rssi(const uint8_t *f) { return (int8_t)f[8]; }
static inline uint32_t fm1_wifi_mac_rd(uint32_t r) { (void)r; return 0; }
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
