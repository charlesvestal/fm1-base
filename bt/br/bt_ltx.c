/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The audio link's TX side (docs/bt/BT-CLASSIC.md §5.5, §7): the LMP / L2CAP signalling into the engine's two TX
 * buffers ahead of the stream's packets, and the AFH switch at its instant. Included by bt_page.c; portable C,
 * host-tested (tests/bt/bt_ltx_test.c, the CPU-load stress model). Ported from the same fix in a firmware's own copy
 * (0acadbc2), the stream's queue left where it is (bt_page.c bp_mq, lent by the app with FM1B_BTA_MEM).
 *
 *   blt_put(pdu, n, llid)   the main loop queues an LMP PDU (LLID 3) or an L2CAP fragment (2 start, 1 continuation)
 *   blt_publish()           the main loop, once the handler that queued is over (bt_inq.c bti_poll, bp_tx_service):
 *                           the queued PDUs may go (an E0 key loaded after its PDU was queued goes first, bp_e0)
 *   blt_fill(f, tab)        the hand-over: the published signalling first (DM1), else the stream's packets (DH5 / DH3 /
 *                           DH1). From IRQ 40 (each reception and the 18-slot alarm, priority 5, above the audio
 *                           render) and from the main loop with IRQ 40 held
 *   blt_afh_stage(..)       the main loop: the next hop set (table, map words, on / off), its LMP_set_AFH queued
 *   blt_afh_alarm(now, ..)  the 18-slot alarm: at the instant the hop set goes to the engine -> slots to the next alarm
 *
 * Why (2026-11-01, on an FM-1, CPU0 at 100 %: Bluetooth audio dropped the connection a few times):
 * the main loop put one signalling PDU a pass into a TX buffer, and LMP_set_AFH's instant (320 slots, 200 ms) was
 * fixed when the PDU was queued. The AFH assessment runs every 30 s. With the app's render holding CPU0, the main
 * loop's passes come hundreds of ms apart: the PDU left after its instant, our side switched alone (the alarm is IRQ
 * 40's, on time), the headset hopped on the old set and the link died (4 s without a good packet, or the headset's own
 * supervision timeout). The host model shows it [I: not seen in a hardware log]. Now: the instant is the hand-over's
 * clock + 320 slots, wherever the main loop is, and the table and the map registers are written at the instant (as
 * the vendor controller does from a slot timer), not at the announcement over a hop set still in use.
 *
 * The includer defines, before including this file:
 *   BLT_CLKN()                 the slot clock (CLKN)
 *   BLT_AFH_MAP(words)         map registers 0 (they go with hop table 1)
 *   BLT_OFF_TX0 / BLT_OFF_TX1  the two DM1 TX payloads in the table
 *   BLT_MEDIA_READY()          an audio packet waits (and its memory is there)
 *   BLT_MEDIA_LOAD(f, tab, i)  the next audio packet into TX buffer i (DH5 / DH3 / DH1)
 * and may define BLT_NOTE_SIG(llid) (a signalling PDU handed over) and BLT_NOTE_BUSY() (audio waits, both buffers
 * held by the engine). struct fm1_br_frame and FM1_BR_OFF_FRQ are hal/fm1_bredr.h's. */
#include <stdint.h>
#include "bt_lmp.h"

#ifndef BLT_NOTE_SIG
#define BLT_NOTE_SIG(llid) ((void)(llid))
#endif
#ifndef BLT_NOTE_BUSY
#define BLT_NOTE_BUSY() ((void)0)
#endif
#define BLT_Q 32u                                  /* signalling PDUs queued (DM1 payloads) */
#define BLT_AFH_AHEAD 320u                         /* the switch this many slots (200 ms) after the hand-over */
#define BLT_CLK 0x07FFFFFFu                        /* 27 bits of slots (FM1_BR_CLK_MASK) */
#define BLT_AFH_OP ((uint8_t)(BTL_SET_AFH << 1))   /* LMP_set_AFH's first byte (the master's, TID 0) */

static struct {
    uint8_t q[BLT_Q][BTL_MAX], ql[BLT_Q], qllid[BLT_Q];
    volatile uint32_t w, pub, r;                   /* queued (main loop), may go (main loop), handed over (IRQ 40) */
    uint8_t afh_tab[80];                           /* the next hop set: frq_table[1]'s content, the map words, on / off */
    uint32_t afh_words[3];
    uint8_t afh_on;
    volatile uint8_t afh_state;                    /* BLT_AFH_*: none, queued (no instant yet), due at afh_at */
    volatile uint32_t afh_at;                      /* the instant, CLKN slots (even: a master-to-slave slot) */
    uint32_t afh_switches;                         /* switches made this link (the diagnostics' line) */
} blt;
enum { BLT_AFH_NONE, BLT_AFH_QUEUED, BLT_AFH_DUE };

static void blt_reset(void)                        /* a new link: nothing queued, no switch due (IRQ 40 held or off) */
{
    blt.w = blt.pub = blt.r = 0;
    blt.afh_state = BLT_AFH_NONE;
    blt.afh_switches = 0;
}
static int blt_put(const uint8_t *pdu, uint32_t len, uint32_t llid)   /* the main loop -> 1 queued */
{
    uint32_t i, k = blt.w % BLT_Q;
    if (blt.w - blt.r >= BLT_Q || len == 0 || len > BTL_MAX)
        return 0;
    for (i = 0; i < len; i++)
        blt.q[k][i] = pdu[i];
    blt.ql[k] = (uint8_t)len;
    blt.qllid[k] = (uint8_t)llid;
    if (llid == 3u && pdu[0] == BLT_AFH_OP && len >= 5u)
        blt.afh_state = BLT_AFH_QUEUED;            /* (its instant set at the hand-over) */
    __asm__ volatile("" ::: "memory");             /* (the slot before the index: RING_PUBLISH) */
    blt.w++;
    return 1;
}
static void blt_publish(void)                      /* the main loop: what is queued may go */
{
    __asm__ volatile("" ::: "memory");
    blt.pub = blt.w;
}

/* one signalling PDU into TX buffer i: DM1 (an LMP_set_AFH gets its instant now) */
static void blt_load_sig(struct fm1_br_frame *f, uint8_t *tab, uint32_t i)
{
    uint32_t k = blt.r % BLT_Q, n = blt.ql[k], j;
    uint8_t *dst = tab + (i ? BLT_OFF_TX1 : BLT_OFF_TX0);
    if (blt.qllid[k] == 3u && blt.q[k][0] == BLT_AFH_OP && n >= 5u) {
        uint32_t at = (BLT_CLKN() + BLT_AFH_AHEAD) & BLT_CLK & ~1u;
        for (j = 0; j < 4u; j++)
            blt.q[k][1u + j] = (uint8_t)(at >> 8u * j);   /* (Part C 5.2: AFH_instant, the slot as it is, bt_lmp.c) */
        blt.afh_at = at;
        blt.afh_state = BLT_AFH_DUE;
    }
    for (j = 0; j < n; j++)
        dst[j] = blt.q[k][j];
    f->txptr[i] = (uint16_t)(i ? BLT_OFF_TX1 : BLT_OFF_TX0);
    f->txphdr[i] = (uint16_t)(n << 3 | 1u << 2 | blt.qllid[k]);   /* LLID 3 LMP, 2 / 1 L2CAP; FLOW 1 */
    f->linkcntl[i] = (uint16_t)((f->linkcntl[i] & 0x80B0u) | 3u | 0x4000u);   /* DM1 */
    f->txbufcntl[i] &= (uint8_t)~1u;
    BLT_NOTE_SIG(blt.qllid[k]);
    blt.r++;
}
/* every empty TX buffer (BR §5.5: bit 0 of txbufcntl set = empty): the published signalling first, the stream after it
 * (never ahead of a published PDU). IRQ 40, or the main loop with IRQ 40 held */
static void blt_fill(struct fm1_br_frame *f, uint8_t *tab)
{
    uint32_t pass, i;
    for (pass = 0; pass < 2u; pass++) {
        int sig = blt.r != blt.pub;
        if (!sig && !BLT_MEDIA_READY())
            return;
        i = f->txtog & 1u;
        if (!(f->txbufcntl[i] & 1u))
            i ^= 1u;
        if (!(f->txbufcntl[i] & 1u)) {
            if (!sig)
                BLT_NOTE_BUSY();
            return;                                /* (both buffers full: the engine has two to send) */
        }
        if (sig)
            blt_load_sig(f, tab, i);
        else
            BLT_MEDIA_LOAD(f, tab, i);
    }
}

/* the next hop set (the main loop, bt_page.c bp_afh_send): kept here until its instant; its PDU queued by the caller */
static void blt_afh_stage(const uint8_t table[80], const uint32_t words[3], uint32_t on)
{
    uint32_t i;
    for (i = 0; i < 80u; i++)
        blt.afh_tab[i] = table[i];
    for (i = 0; i < 3u; i++)
        blt.afh_words[i] = words[i];
    blt.afh_on = (uint8_t)on;
}
/* the 18-slot alarm (IRQ 40): at the instant the staged hop set goes to the engine (frq_table[1], map 0, miscntl [7:6],
 * format bit 6; BR §7) -> the slots to arm the next alarm with (the instant's own when it comes sooner than 18) */
static uint32_t blt_afh_alarm(uint32_t now, struct fm1_br_frame *f, uint8_t *tab)
{
    uint32_t left, i;
    if (blt.afh_state != BLT_AFH_DUE)
        return 18u;
    left = (blt.afh_at - now) & BLT_CLK;
    if (left == 0 || left > (BLT_CLK >> 1)) {      /* (at or past it) */
        if (blt.afh_on) {
            for (i = 0; i < 80u; i++)
                tab[FM1_BR_OFF_FRQ + 80u + i] = blt.afh_tab[i];
            BLT_AFH_MAP(blt.afh_words);
        }
        f->miscntl = (uint8_t)((f->miscntl & 0x3Fu) | (blt.afh_on ? 1u << 6 : 0u));   /* hop table 1 / basic (BR §4) */
        f->format = (uint8_t)((f->format & 0xBFu) | (blt.afh_on ? 0x40u : 0u));
        blt.afh_state = BLT_AFH_NONE;
        blt.afh_switches++;
        return 18u;
    }
    return left < 18u ? left : 18u;
}
