/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* MIDI OUT on the SMK-37's TRS jack (boards/smk37/fm1_uart.h: UART1 TX on PC7, 31250 baud; the jack has no MIDI
 * IN). A 256-byte ring fed by plat_midi_send (TO_TRS, and everything that goes to USB: the stream a computer gets,
 * the jack gets too), drained one byte a tick from the TIMER5 ISR (trs_out_tick, 10 kHz: 3.2 bytes a byte time),
 * never waiting: a byte the UART has not taken in TRS_STUCK_TICKS is dropped, and after TRS_STUCK_MAX of those in a
 * row the jack goes quiet (a UART that does not answer never stalls the tick). Status every message (no running
 * status), real-time bytes as they come. Not yet tried on a unit. */
#include "fm1_uart.h"

#define TRS_RING 256u
#define TRS_STUCK_TICKS 30u                       /* 3 ms: a byte should take 0.32 ms */
#define TRS_STUCK_MAX 4u
static volatile uint8_t trs_ring[TRS_RING];
static volatile uint32_t trs_w, trs_r;
static uint32_t trs_bytes, trs_drops;              /* sent; not queued (the ring full) or abandoned */
static uint8_t trs_wait, trs_stuck, trs_off, trs_user_off;

static void uart_midi_init(void) { fm1_uart1_tx_init(); }   /* before TIMER5 starts (an IOMAP read-modify-write) */
static void uart_midi_poll(void) {}                /* (the FM-1's MIDI IN poll: nothing comes in; the out goes by the tick) */

static uint32_t trs_len(uint32_t pkt)             /* a packet's byte count from its CIN; 0: not for the jack (SysEx, a
                                                    * status that does not fit its CIN, a data byte with bit 7) */
{
    uint32_t cin = pkt & 0xFu, st = (pkt >> 8) & 0xFFu, n;
    if (cin >= 8u && cin <= 14u) {                 /* channel voice */
        if (st >> 4 != cin)
            return 0u;
        n = cin == 12u || cin == 13u ? 2u : 3u;
    } else if (cin == 15u) {                       /* a single byte: real time only */
        return st >= 0xF8u && st != 0xF9u && st != 0xFDu ? 1u : 0u;
    } else if (cin == 2u) {                        /* MTC quarter frame, song select */
        if (st != 0xF1u && st != 0xF3u)
            return 0u;
        n = 2u;
    } else if (cin == 3u) {                        /* song position */
        if (st != 0xF2u)
            return 0u;
        n = 3u;
    } else {
        return 0u;
    }
    if ((pkt >> 16) & 0x80u || (n == 3u && (pkt >> 24) & 0x80u))
        return 0u;
    return n;
}
void plat_trs_out(int on)                         /* the jack on or off; on tries a UART that had gone quiet again */
{
    trs_user_off = (uint8_t)!on;
    trs_off = 0;
    trs_stuck = 0;
}
static void trs_out_put(uint32_t pkt)             /* main loop or the tick, under plat_midi_send's lock */
{
    uint32_t n = trs_len(pkt), i;
    if (!n || trs_off || trs_user_off)
        return;
    if (TRS_RING - (trs_w - trs_r) < n) {
        trs_drops++;
        return;
    }
    for (i = 0; i < n; i++)
        trs_ring[(trs_w + i) % TRS_RING] = (uint8_t)(pkt >> (8u * (i + 1u)));
    RING_PUBLISH();
    trs_w += n;
}
static void trs_out_tick(void)                    /* TIMER5: one byte if the UART took the last */
{
    if (trs_r == trs_w)
        return;
    if (!fm1_uart1_tx_ready()) {
        if (++trs_wait < TRS_STUCK_TICKS)
            return;
        trs_wait = 0;                              /* the UART never took it: the byte goes, and after a few */
        trs_drops++;                               /* the jack is given up until the next init */
        trs_r++;
        if (++trs_stuck >= TRS_STUCK_MAX)
            trs_off = 1;
        return;
    }
    fm1_uart1_tx_byte(trs_ring[trs_r % TRS_RING]);
    trs_r++;
    trs_bytes++;
    trs_wait = 0;
    trs_stuck = 0;
}
