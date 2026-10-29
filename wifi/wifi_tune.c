/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David (hdavid), Optimist (github.com/w0ts/optimist) */
/* The Wi-Fi tuning's pure part (wifi_tune.h): no radio, no flash. Host-tested in tests/wifi/wifi_tune_test.c. */
#include "wifi_tune.h"

uint32_t wt_mask_from(uint8_t kept, uint32_t def) { return (kept & WT_KEPT) ? kept & WT_ALL : def & WT_ALL; }
uint8_t wt_mask_keep(uint32_t mask) { return (uint8_t)(WT_KEPT | (mask & WT_ALL)); }

/* ---- (a) */
static uint32_t wt_crc16(uint32_t crc, const uint8_t *p, uint32_t n)   /* CRC-16/XMODEM: poly 0x1021, init 0 */
{
    uint32_t k;
    while (n--) {
        crc ^= (uint32_t)*p++ << 8;
        for (k = 0; k < 8u; k++)
            crc = crc & 0x8000u ? (crc << 1) ^ 0x1021u : crc << 1;
        crc &= 0xFFFFu;
    }
    return crc;
}
int wt_187_ok(const uint8_t r[WT_187_LEN])
{
    uint32_t c = (uint32_t)r[64] | (uint32_t)r[65] << 8;
    return c != 0u && wt_crc16(0, r, 64) == c;     /* (wifi_trim_is_already: trim_mage 0 is no record) */
}
int wt_vm_187(wt_read_fn rd, void *ctx, uint32_t area, uint32_t size, uint8_t out[WT_187_LEN])
{
    static const uint8_t magic[4] = {0x55, 0xAA, 0xAA, 0x55};
    uint8_t h[4], d[32], rec[WT_187_LEN];
    uint32_t off = 4, id, len, pos, k, crc, i;
    int found = 0;
    if (rd(ctx, area, h, 4))
        return 0;
    for (i = 0; i < 4u; i++)
        if (h[i] != magic[i])
            return 0;
    while (off + 4u <= size) {
        if (rd(ctx, area + off, h, 4))
            return found;
        id = h[1] | (uint32_t)(h[2] & 0x0Fu) << 8;
        len = (uint32_t)(h[2] >> 4) | (uint32_t)h[3] << 4;
        if (off + 4u + len > size)
            break;                                  /* (past the area: the end) */
        for (pos = 0, crc = 0; pos < len; pos += k) {
            k = len - pos < sizeof d ? len - pos : (uint32_t)sizeof d;
            if (rd(ctx, area + off + 4u + pos, d, k))
                return found;
            crc = wt_crc16(crc, d, k);
            if (id == 187u && len == WT_187_LEN)
                for (i = 0; i < k; i++)
                    rec[pos + i] = d[i];
        }
        if ((crc & 0xFFu) != h[0])
            break;                                  /* (the first failing check ends the log: an erased header always fails) */
        if (id == 187u && len == WT_187_LEN && wt_187_ok(rec)) {
            for (i = 0; i < WT_187_LEN; i++)
                out[i] = rec[i];                    /* (the last valid one wins) */
            found = 1;
        }
        off += 4u + len;
    }
    return found;
}

/* ---- (c) */
static const int8_t wt_agc_edge[4] = {-36, -46, -60, -76};   /* a signal above edge i: step i */
static const uint8_t wt_agc_gain[5] = {34, 43, 51, 57, 66};
static uint32_t wt_agc_idx(int32_t rssi)
{
    uint32_t i;
    for (i = 0; i < 4u && rssi <= wt_agc_edge[i]; i++)
        ;
    return i;
}
uint32_t wt_agc_step(int32_t rssi) { return wt_agc_gain[wt_agc_idx(rssi)]; }
uint32_t wt_agc_next(uint32_t gain, int32_t rssi)
{
    uint32_t cur = 4, weaker = wt_agc_idx(rssi + WT_AGC_HYST), stronger = wt_agc_idx(rssi - WT_AGC_HYST), i;
    for (i = 0; i < 5u; i++)
        if (wt_agc_gain[i] == gain)
            cur = i;                                /* (a gain not of the steps: as the start's, 66) */
    if (weaker > cur)
        return wt_agc_gain[weaker];                 /* clearly weaker than the step's band: more gain */
    if (stronger < cur)
        return wt_agc_gain[stronger];               /* clearly stronger: less */
    return wt_agc_gain[cur];
}

/* ---- (f) */
void wt_rate_reset(struct wt_rate *r, uint32_t now_ms)
{
    r->slow = 0;
    r->k = 0;
    r->t_until = 0;
    r->t_fast = now_ms;
}
void wt_rate_second(struct wt_rate *r, uint32_t now_ms, uint32_t lost)
{
    if (r->slow) {
        if ((int32_t)(now_ms - r->t_until) >= 0) {
            r->slow = 0;                            /* the hold is over: the fast rate again */
            r->t_fast = now_ms;
        }
        return;
    }
    if (lost < WT_RATE_LOST_N)
        return;
    if (now_ms - r->t_fast < WT_RATE_SOON_S * 1000u) {
        if (r->falls && r->k < 3u)
            r->k++;                                 /* (it fell again soon after coming back: a longer hold) */
    } else {
        r->k = 0;
    }
    r->slow = 1;
    r->t_until = now_ms + (WT_RATE_HOLD_S * 1000u << r->k);
    r->falls++;
}
int wt_rate_fast(const struct wt_rate *r, int on, const uint8_t *f, uint32_t len, uint32_t ack)
{
    uint32_t hl;
    if (!on || r->slow || !ack || len < 24u || (f[0] & 0x0Cu) != 0x08u || (f[4] & 1u))
        return 0;                                   /* (off, held back, no ACK, not data, group-addressed) */
    hl = (f[0] & 0x80u) ? 26u : 24u;                /* (QoS data: two bytes more) */
    if (len >= hl + 8u && f[hl + 6u] == 0x88u && f[hl + 7u] == 0x8Eu)
        return 0;                                   /* EAPOL (the keys): at 1 Mb/s, as the join's management frames */
    return 1;
}
