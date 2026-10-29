/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0
 * Copyright (C) 2026 Henri David (hdavid), Optimist (github.com/w0ts/optimist) */
/* The own Wi-Fi stack's tuning on the radio (wifi/wifi_tune.h: what each switch is and where JieLi's SDK does it), part
 * of device/wifi_own.c (the unity build), after its quality counters and before the service. Every switch acts at once
 * (plat_wifi_tune_set), on and off, so SYSTEM > WI-FI's numbers can be compared within a minute:
 *   (a) WT_CAL   at the Wi-Fi side's start (own_radio_up) and when switched: the stored 187's IQ fields into the PHY
 *                (felucca-bsp fm1_ble_rf.h fm1_rf_cal_wifi_iq), or the start's own back; the VM read once.
 *   (b) WT_FILT  each pass: the station filter while associated (WC_HS .. WC_UP, not MODE AP), else everything in, written
 *                only when that changes (and nothing at all while the switch stays off).
 *   (c) WT_AGC   each pass while associated: the gain from the AP's signal mean (wt_agc_next), written on a change only;
 *                66 again when the AP goes quiet, the join ends or the switch goes off.
 *   (f) WT_RATE  each frame sent: its rate (wt_rate_fast); once a second while joined (or MODE AP up): the MAC's
 *                never-acknowledged count into the fallback (wt_rate_second), the counters shared with plat_wifi_quality's
 *                window. The MAC's fallback table (JieLi's) written once, the first time the switch is on with the radio up.
 * With all four off the stack writes exactly what it wrote before. */
#ifndef FM1B_WIFI_TUNE
#define FM1B_WIFI_TUNE WT_ALL            /* the switches on when the record chose none (an app may say otherwise) */
#endif
#define WT_FAST_W0 FM1_WIFI_RATE_CCK2    /* (f)'s rate: see wifi_tune.h and the report on fm1-next-wififix for the choice */
#define WT_FAST_KBPS 2000u

static struct {
    uint8_t up;                          /* the Wi-Fi side is up (own_radio_up) */
    uint8_t cal;                         /* (a) the VM: 0 not read yet, 1 a valid 187 read, 2 none */
    uint8_t cal_in;                      /* .. its fields are in the PHY now */
    uint8_t filt;                        /* (b) the station filter is in */
    uint8_t gain;                        /* (c) the highest AGC gain in the PHY (once up) */
    uint8_t fbk;                         /* (f) the MAC's fallback table written */
    uint8_t cnt;                         /* the MAC's counters are being read once a second */
    uint8_t joined;                      /* associated last pass (the rate's state starts over at a new join) */
    int16_t ema;                         /* the AP's signal, 1/16 dBm (0: none yet this join) */
    uint32_t t_heard, t_sec;
    uint32_t n_fast, n_cal;              /* frames sent at the fast rate; calibration writes */
    struct wt_rate rate;
    uint8_t x187[WT_187_LEN];
} wt;

static uint32_t wt_mask(void) { return wt_mask_from(wn_st.rsv, FM1B_WIFI_TUNE); }
static int wt_assoc(void)                /* the station is associated (keys, DHCP or up): (b), (c) apply */
{
#if FM1B_WIFI_AP
    if (wl.ap)
        return 0;
#endif
    return wc.st == WC_HS || wc.st == WC_LINK || wc.st == WC_UP;
}
static int wt_rate_on(void)              /* (f) applies: joined, or the access point up */
{
    int ap = 0;
#if FM1B_WIFI_AP
    ap = wl.ap && wl.on;
#endif
    return (wt_mask() & WT_RATE) && (wc.st == WC_UP || ap);
}

/* ---- the radio's side: the BBP window, the calibration, the flash */
#ifdef PLAT_HOST
static int wt_flash(void *ctx, uint32_t off, uint8_t *dst, uint32_t n)
{
    (void)ctx;
    return fm1_fake_flash_rd(off, dst, n);
}
#else
static int wt_flash(void *ctx, uint32_t off, uint8_t *dst, uint32_t n)   /* SPI reads (st_read), as own_mac's */
{
    (void)ctx;
    return flash_ok ? st_read(off, dst, n) : -1;
}
#endif
static void wt_cal_apply(void)           /* (a) as the switch says, once the Wi-Fi side is up */
{
    int want = (wt_mask() & WT_CAL) != 0u;
    if (!wt.up)
        return;
    if (want && !wt.cal)
        wt.cal = (uint8_t)(wt_vm_187(wt_flash, 0, WT_VM_AREA, WT_VM_SIZE, wt.x187) ? 1u : 2u);
    if (want && wt.cal == 1u && !wt.cal_in) {
        wt.n_cal += fm1_rf_cal_wifi_iq(wt.x187);
        wt.cal_in = 1;
    } else if (!want && wt.cal_in) {
        wt.n_cal += fm1_rf_cal_wifi_iq(0);   /* (the start's own values back) */
        wt.cal_in = 0;
    }
}
static void wt_gain(uint32_t g)         /* (c)'s register, written on a change only and only with the radio up */
{
    if (!wt.up || g == wt.gain)
        return;
    fm1_bbp_win_wr(FM1_WIFI_AGC_WIN, FM1_WIFI_AGC_ENTRY, g);
    wt.gain = (uint8_t)g;
}
static void wt_radio_up(void)            /* the Wi-Fi side just came up (fm1_wifi_up) */
{
    wt.up = 1;
    wt.gain = FM1_WIFI_AGC_START;        /* (the start's value: nothing written while (c) has nothing to change) */
    wt_cal_apply();
    if ((wt_mask() & WT_RATE) && !wt.fbk) {
        fm1_wifi_fallback_sdk();
        wt.fbk = 1;
    }
}
static void wt_heard(int8_t rssi)        /* a frame of the joined AP (wl_rx, wc_on_join): its signal into the mean */
{
    int32_t e = wt.ema;
    if (rssi >= 0)
        return;
    e = e ? e + ((int32_t)rssi * 16 - e) / 8 : (int32_t)rssi * 16;   /* (as oq_signal's: the last ~8 frames) */
    wt.ema = (int16_t)(e < -2047 ? -2047 : e > -1 ? -1 : e);
    wt.t_heard = own_ms();
}

/* ---- the MAC's counters once a second, for plat_wifi_quality's window and (f)'s fallback (they clear when read) */
static void wt_cnt_read(uint32_t now)
{
    uint32_t c0 = fm1_wifi_mac_rd(OQ_TX_STA_CNT0), c1 = fm1_wifi_mac_rd(OQ_TX_STA_CNT1), ov = fm1_wifi_mac_rd(OQ_RX_STA_CNT2);
    wt.t_sec = now;
    if (oq.on) {
        oq.ok[oq.k] = (uint16_t)c1;
        oq.rty[oq.k] = (uint16_t)(c1 >> 16);
        oq.fail[oq.k] = (uint16_t)c0;
        oq.over[oq.k] = (uint16_t)(ov >> 16);
        oq.k = (uint8_t)((oq.k + 1u) % OQ_SLOTS);
        if (oq.n < OQ_SLOTS)
            oq.n++;
    }
    if (wt_rate_on())
        wt_rate_second(&wt.rate, now, c0 & 0xFFFFu);
}
static uint8_t wt_cnt_running(void) { return wt.cnt; }
static void wt_cnt_poll(void)            /* oq_poll's: a second's counts while measuring or while (f) watches */
{
    uint32_t now = own_ms();
    if (oq.on && now - oq.t_ask >= OQ_HOLD_MS)
        oq.on = 0;
    if (!oq.on && !wt_rate_on()) {
        wt.cnt = 0;
        return;
    }
    if (!wt.cnt) {                       /* (the counts from before are not this window's) */
        oq_clear_counters();
        wt.cnt = 1;
        wt.t_sec = now;
        return;
    }
    if (now - wt.t_sec >= 1000u)
        wt_cnt_read(now);
}

/* ---- each pass (wifi_poll) */
static void wt_poll(void)
{
    uint32_t m = wt_mask(), now = own_ms();
    int assoc = wt_assoc(), want;
    if (assoc && !wt.joined) {           /* a new join: its own signal mean, the fast rate to begin with */
        wt.ema = 0;
        wt.t_heard = now;
        wt_rate_reset(&wt.rate, now);
    }
    wt.joined = (uint8_t)assoc;
    want = assoc && (m & WT_FILT);       /* (b) */
    if (want != wt.filt) {
        fm1_wifi_mac_wr(FM1_WIFI_RX_FILTER, want ? FM1_WIFI_RX_FILTER_STA : FM1_WIFI_RX_FILTER_ALL);
        wt.filt = (uint8_t)want;
    }
    if (assoc && (m & WT_AGC))           /* (c) */
        wt_gain(!wt.ema || now - wt.t_heard >= WT_AGC_IDLE_MS ? WT_AGC_FREE : wt_agc_next(wt.gain, (wt.ema - 8) / 16));
    else
        wt_gain(WT_AGC_FREE);
    if (wt.up && (m & WT_RATE) && !wt.fbk) {   /* (f): its table, the first time it is wanted */
        fm1_wifi_fallback_sdk();
        wt.fbk = 1;
    }
    wt_cnt_poll();
}
/* a frame out of wifi_txb: at (f)'s rate when it applies */
static void wt_prepare(uint32_t len, uint32_t wcid, uint32_t ack)
{
    int fast = wt_rate_fast(&wt.rate, wt_rate_on(), own_frame(), len, ack);
    fm1_wifi_tx_prepare_rate(wifi_txb, len, wcid, ack, fast ? WT_FAST_W0 : FM1_WIFI_RATE_CCK1);
    wt.n_fast += (uint32_t)fast;
}
static uint32_t wt_kbps(void) { return wt_rate_on() && !wt.rate.slow ? WT_FAST_KBPS : 1000u; }

/* ---- plat.h */
uint32_t plat_wifi_tune_get(void) { own_store_load(); return wt_mask(); }
void plat_wifi_tune_set(uint32_t mask)
{
    own_store_load();
    if (wt_mask_keep(mask) == wn_st.rsv)
        return;
    wn_st.rsv = wt_mask_keep(mask);
    own_store_changed();
    wt_cal_apply();                      /* ((b), (c), (f) at the next pass) */
}
int plat_wifi_tune_info(struct plat_wifi_tune *t)
{
    own_store_load();
    t->on = (uint8_t)wt_mask();
    t->cal = wt.cal;
    t->cal_in = wt.cal_in;
    t->filt_sta = wt.filt;
    t->agc_gain = wt.gain;
    t->rate_slow = wt.rate.slow;
    t->tx_kbps = (uint16_t)wt_kbps();
    t->falls = wt.rate.falls;
    t->fast = wt.n_fast;
    return wt.up;
}
