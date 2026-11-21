/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Host stress test of Bluetooth audio's TX side under CPU load (bt/br/bt_ltx.c, the code as built): the link must hold
 * whatever the app's render does to CPU0 (2026-11-01, on an FM-1: Bluetooth audio was dropping
 * the connection a few times; CPU0 at 100 %). Ported with the fix from a firmware's own
 * tests/bt_ltx_test.c (0acadbc2); the stream's queue here is bt_page.c's (BLT_MEDIA_READY / BLT_MEDIA_LOAD).
 *
 * The model, slot by slot (625 us), 10 minutes a run:
 *   - the engine (BR §5.5): the master sends its TX buffer txtog (DH5 = 6 slots with the answer, DM1 / POLL = 2), the
 *     headset answers a share of them (the rest lost: the buffer is sent again, ARQ); an answer frees the buffer,
 *     flips txtog and raises IRQ 40's RX interrupt; the 18-slot alarm is IRQ 40's too. IRQ 40 is priority 5 during a
 *     run, above the audio (3): it runs in its slot whatever the load (bt_inq.c BTI_RUN_PRIO)
 *   - the app's render at a CPU0 load of 50 %, 100 %, 130 % and 200 %: over 100 % halves are late and their audio is
 *     never encoded (fewer packets queued); the main loop gets a pass every ~15 ms under the ceiling, and at 100 % and
 *     over only now and then, 0.2-1.5 s apart
 *   - the main loop's BT work: every 30 s an AFH switch (bt_page.c bp_afh_send: blt_afh_stage, LMP_set_AFH queued with
 *     instant 0), now and then an L2CAP answer of 3 fragments (bt_acl.c ba_send), published at the pass's end
 *   - the headset switches its hop set at the instant of the LMP_set_AFH it received; received after its instant, the
 *     two sides hop apart and the link is lost (counted, as is any 4 s without a good packet: bt_page.c's supervision)
 * Checks, at each load: no AFH switch apart, no 4 s silence, every published signalling PDU handed over within 2 alarm
 * periods (36 slots, 22.5 ms) whatever the main loop does, each instant set at its hand-over and the engine switched on
 * it, the stream's packets keep flowing (over 90 % of those the render made go out). And the old TX side (the instant
 * fixed when queued, the signalling put into a TX buffer by the main loop only, one PDU a pass) at 100 % and over:
 * switches apart, the cause this fixes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* the table's offsets and the frame control block as hal/fm1_bredr.h has them (BR §3, §4): the fields bt_ltx.c uses */
#define FM1_BR_OFF_FRQ 32u
#define FM1_BR_OFF_FRAME 368u
struct fm1_br_frame {
    volatile uint16_t bdaddr[3], bch_lo, bch_mid;
    volatile uint8_t bch_hi, cx, miscntl, format, rxtog, txtog;
    volatile uint16_t wincntl, rxmaxbuf;
    volatile uint8_t txpwr, txfreq, rxrssi, rxfreq;
    volatile uint16_t txptr[2], rxptr[2], linkcntl[2], txphdr[2], rxstat[2], rxphdr[2];
    volatile uint16_t clk_off_lo, clk_off_hi, rx_clkn_lo, rx_clkn_hi, bitoff, rxbit;
    volatile uint16_t cryptk[8], escorxstat[2], lmprxptr[2], aclprio, escoprio, bufcntl;
    volatile uint8_t txbufcntl[2], rxbufcntl[2], rxbufstat[2];
    volatile uint16_t tws[5];
    volatile uint8_t twssta[2];
    volatile uint32_t txpwer, rxgain0, txset, rxset, pll_comp, mdm_set;
    volatile uint16_t rssi[4];
    volatile uint32_t anl_out;
    volatile uint8_t tws_copy[36];
};
_Static_assert(sizeof(struct fm1_br_frame) == 180u && __builtin_offsetof(struct fm1_br_frame, linkcntl) == 32u &&
                   __builtin_offsetof(struct fm1_br_frame, txbufcntl) == 90u,
               "fm1_br_frame: BR §4's offsets");

static uint32_t clk;                                    /* CLKN, slots */
static uint32_t map_writes, map_words[3];
/* the stream's queue (bt_page.c bp_mq): lengths only, the packets' bytes do not matter here */
#define MQ 40u
static struct {
    uint16_t n[MQ];
    uint32_t w, r, busy;
} mq;
static void media_load(struct fm1_br_frame *f, uint8_t *tab, uint32_t i)   /* bt_page.c bp_media_load */
{
    uint32_t n = mq.n[mq.r % MQ], t = n > 183u ? 15u : n > 27u ? 11u : 4u;
    (void)tab;
    f->txphdr[i] = (uint16_t)(n << 3 | 1u << 2 | 2u);
    f->linkcntl[i] = (uint16_t)((f->linkcntl[i] & 0x80B0u) | t | 0x4000u);
    f->txbufcntl[i] &= (uint8_t)~1u;
    mq.r++;
}
#define BLT_CLKN() clk
#define BLT_AFH_MAP(w) (map_writes++, memcpy(map_words, (w), sizeof map_words))
#define BLT_OFF_TX0 4272u                               /* (bt_page.c BP_OFF_TX0 / 1) */
#define BLT_OFF_TX1 4292u
#define BLT_MEDIA_READY() (mq.r != mq.w)
#define BLT_MEDIA_LOAD(f, tab, i) media_load((f), (tab), (i))
#define BLT_NOTE_BUSY() (mq.busy++)
#include "../../bt/br/bt_lmp.c"
#include "../../bt/br/bt_ltx.c"

static int fails;
static void check(const char *what, int ok)
{
    printf("%-112s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

static uint32_t rng = 1;
static uint32_t rnd(uint32_t n)
{
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % n;
}

static uint8_t tab[4480u + 2u * 344u];
#define SLOTS_S 1600u
#define RUN_S 600u
#define HALF_SLOTS 9u                                   /* an audio half (256 frames, 5.8 ms) in slots, about */
#define PKT_EVERY 14u                                   /* a 3-frame SBC packet each 384 samples: 8.7 ms, ~14 slots */

struct run {
    uint32_t afh_sent, afh_apart, afh_on_time, silences, sig_max_wait, instant_short, pkts_made, pkts_sent, sig_sent;
    uint32_t max_gap, afh_late_rx;
};

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* the old TX side, as it was until this port: the main loop's bp_tx_service put one PDU a pass into a TX buffer */
static void old_service(struct fm1_br_frame *f)
{
    uint32_t i = f->txtog & 1u;
    if (!(f->txbufcntl[i] & 1u))
        i ^= 1u;
    if (blt.r != blt.w && (f->txbufcntl[i] & 1u)) {
        uint32_t k = blt.r % BLT_Q, n = blt.ql[k];
        memcpy(tab + (i ? BLT_OFF_TX1 : BLT_OFF_TX0), blt.q[k], n);
        f->linkcntl[i] = 0x4003u;
        f->txbufcntl[i] &= (uint8_t)~1u;
        blt.r++;
    }
}

/* the main loop's pass: the AFH switch every 30 s, now and then an L2CAP answer, then the hand-over */
static void main_pass(struct run *R, struct fm1_br_frame *f, int old, uint32_t *last_afh, uint32_t *old_at,
                      uint32_t *old_due, uint32_t pub_at[BLT_Q])
{
    uint8_t out[BTL_MAX + 8], map[10];
    if (clk - *last_afh >= 30u * SLOTS_S) {             /* bp_sec_tick -> bp_afh_assess -> bp_afh_send */
        uint8_t t80[80];
        uint32_t w3[3] = {1, 2, 3};
        *last_afh = clk;
        memset(map, 0xFF, 9), map[9] = 0x7F;
        memset(t80, 0x11, sizeof t80);
        if (!old && blt.afh_state == BLT_AFH_NONE) {
            blt_afh_stage(t80, w3, 1);
            R->afh_sent += (uint32_t)blt_put(out, btl_set_afh(out, 0, 1, map), 3u);
        } else if (old && !*old_due) {
            *old_at = (clk + 320u) & ~1u;               /* (fixed when queued, as bp_afh_send did) */
            *old_due = 1;
            R->afh_sent += (uint32_t)blt_put(out, btl_set_afh(out, *old_at, 1, map), 3u);
        }
    }
    if (rnd(8) == 0) {                                  /* an L2CAP answer, 3 DM1 fragments (ba_send) */
        memset(out, 0x5A, sizeof out);
        (void)blt_put(out, BTL_MAX, 2u), (void)blt_put(out, BTL_MAX, 1u), (void)blt_put(out, 5u, 1u);
    }
    if (!old) {
        uint32_t i;
        for (i = blt.pub; i != blt.w; i++)
            pub_at[i % BLT_Q] = clk;
        blt_publish();                                  /* (bti_poll's end; bp_tx_service's too) */
        blt_fill(f, tab);
    } else
        old_service(f);
}

/* one run: load in percent; old = the TX side as it was before the port */
static struct run simulate(uint32_t load, int old)
{
    struct run R;
    struct fm1_br_frame *f = (struct fm1_br_frame *)(void *)(tab + FM1_BR_OFF_FRAME);
    uint32_t end = RUN_S * SLOTS_S, busy_until = 0, next_pass = 0, last_afh = 0, last_good = 0, next_alarm = 18u;
    uint32_t pkt_acc = 0, half_acc = 0, pub_at[BLT_Q];
    uint32_t x_buf = 0, x_len = 0, x_pkt = 0;           /* the exchange on air: which buffer, a packet in it, its kind */
    uint8_t x_head[5];
    uint32_t m_gen = 0, s_gen = 0, s_next_at = 0, s_pending = 0, old_at = 0, old_due = 0;   /* each side's hop set */
    memset(&R, 0, sizeof R);
    memset(tab, 0, sizeof tab);
    memset(&blt, 0, sizeof blt);
    memset(&mq, 0, sizeof mq);
    memset(x_head, 0, sizeof x_head);
    memset(pub_at, 0, sizeof pub_at);
    rng = 12345u + load * 7u + (uint32_t)old;
    f->txbufcntl[0] = f->txbufcntl[1] = 1;              /* both empty */
    blt_reset();
    for (clk = 0; clk < end; clk++) {
        /* ---- the render (priority 3): over 100 % only 100/load of its halves are on time and encoded */
        if (++half_acc >= HALF_SLOTS) {
            half_acc = 0;
            pkt_acc += load <= 100u ? 100u : 10000u / load;
        }
        while (pkt_acc >= 100u * PKT_EVERY / HALF_SLOTS) {
            pkt_acc -= 100u * PKT_EVERY / HALF_SLOTS;
            R.pkts_made++;
            if (mq.w - mq.r < MQ) {                     /* (bt_page.c bp_media_queue; full: dropped whole) */
                mq.n[mq.w % MQ] = 332u;
                mq.w++;
            }
        }
        /* ---- the main loop: a pass when the render leaves it one */
        if (clk >= next_pass) {
            main_pass(&R, f, old, &last_afh, &old_at, &old_due, pub_at);
            next_pass = clk + (load < 100u ? 24u : (200u + rnd(1300u)) * SLOTS_S / 1000u);
        }
        /* ---- the 18-slot alarm (IRQ 40, priority 5: on time whatever the load) */
        if (clk >= next_alarm) {
            uint32_t was = f->miscntl & 0x40u;
            if (!old) {
                next_alarm = clk + blt_afh_alarm(clk, f, tab);
                blt_fill(f, tab);
            } else {                                    /* the old bp_alarm: the switch at the queued instant */
                uint32_t left = old_at - clk;
                if (old_due && (int32_t)left <= 0) {
                    f->miscntl |= 0x40u;
                    old_due = 0;
                    left = 18u;
                }
                next_alarm = clk + (old_due && left < 18u ? left : 18u);
            }
            if ((f->miscntl & 0x40u) != was) {          /* our side switched */
                f->miscntl &= (uint8_t)~0x40u;          /* (the model's next switch counts again) */
                m_gen++;
                if (!old && blt.afh_at != clk)
                    R.instant_short++;                  /* (the engine switches on the instant itself) */
            }
        }
        if (s_pending && clk == s_next_at) {            /* the headset switches at the instant it got */
            s_gen++;
            s_pending = 0;
        }
        /* ---- the engine: one exchange after the other; the headset answers 85 % of them while both hop alike */
        if (clk >= busy_until) {
            if (x_len && m_gen == s_gen && rnd(100) < 85u) {
                if (x_pkt) {                            /* acknowledged: the buffer free, txtog flipped */
                    if (x_len == 6u)
                        R.pkts_sent++;
                    else {
                        R.sig_sent++;
                        if (x_head[0] == BLT_AFH_OP) {  /* the headset got LMP_set_AFH */
                            uint32_t at = le32(x_head + 1);
                            if (at > clk)
                                s_next_at = at, s_pending = 1;
                            else
                                R.afh_late_rx++;        /* (after its instant: it never switches with us) */
                        }
                    }
                    f->txbufcntl[x_buf] |= 1u;
                    f->txtog ^= 1u;
                }
                if (clk - last_good > R.max_gap)
                    R.max_gap = clk - last_good;
                last_good = clk;
                {                                       /* IRQ 40 RX: bp_conn_rx -> blt_fill */
                    uint32_t r0 = blt.r;
                    if (old)
                        blt.pub = blt.r;                /* (the old one handed over media only, never ahead) */
                    if (!old || blt.r == blt.w)
                        blt_fill(f, tab);
                    for (; !old && r0 != blt.r; r0++)
                        if (clk - pub_at[r0 % BLT_Q] > R.sig_max_wait)
                            R.sig_max_wait = clk - pub_at[r0 % BLT_Q];
                }
            }
            x_buf = f->txtog & 1u;
            x_pkt = !(f->txbufcntl[x_buf] & 1u);
            x_len = x_pkt && (f->linkcntl[x_buf] & 15u) == 15u ? 6u : 2u;
            if (x_pkt && x_len == 2u) {
                memcpy(x_head, tab + (x_buf ? BLT_OFF_TX1 : BLT_OFF_TX0), sizeof x_head);
                if (!old && x_head[0] == BLT_AFH_OP && le32(x_head + 1) != blt.afh_at)
                    R.instant_short++;
            } else
                x_head[0] = 0;
            busy_until = clk + x_len;
        }
        if (m_gen != s_gen && !s_pending) {             /* the two hop apart: the link is lost */
            R.afh_apart++;
            s_gen = m_gen;                              /* (the model goes on as a new link would) */
        }
        if (clk - last_good > 4u * SLOTS_S) {
            R.silences++;
            last_good = clk;
        }
    }
    R.afh_on_time = m_gen - R.afh_apart;
    return R;
}

static void switch_at_instant(void)   /* the staged table and map words reach the engine at the instant, not before */
{
    struct fm1_br_frame *f = (struct fm1_br_frame *)(void *)(tab + FM1_BR_OFF_FRAME);
    uint8_t t80[80], out[BTL_MAX + 8], map[10];
    uint32_t w3[3] = {7, 8, 9}, at, r;
    memset(tab, 0, sizeof tab);
    memset(&blt, 0, sizeof blt);
    memset(&mq, 0, sizeof mq);
    map_writes = 0;
    memset(t80, 0x22, sizeof t80);
    memset(map, 0xFF, sizeof map);
    f->txbufcntl[0] = f->txbufcntl[1] = 1;
    clk = 1000u;
    blt_afh_stage(t80, w3, 1);
    (void)blt_put(out, btl_set_afh(out, 0, 1, map), 3u);
    blt_fill(f, tab);
    check("staged, queued, not published: nothing handed over, the table and the map untouched",
          blt.r == 0u && blt.afh_state == BLT_AFH_QUEUED && tab[FM1_BR_OFF_FRQ + 80u] == 0 && !map_writes);
    clk = 1003u;
    blt_publish();
    blt_fill(f, tab);
    at = tab[BLT_OFF_TX0 + 1] | (uint32_t)tab[BLT_OFF_TX0 + 2] << 8;
    check("published: handed over as DM1 with the instant 320 slots (even) after the hand-over", blt.r == 1u &&
          at == 1322u && blt.afh_at == 1322u && blt.afh_state == BLT_AFH_DUE && (f->linkcntl[0] & 15u) == 3u &&
          (f->txphdr[0] & 3u) == 3u);
    clk = 1310u;
    r = blt_afh_alarm(clk, f, tab);
    check("12 slots before the instant: the alarm armed onto it, the engine still on its old hop set",
          r == 12u && !(f->miscntl & 0x40u) && tab[FM1_BR_OFF_FRQ + 80u] == 0 && !map_writes);
    clk = 1322u;
    r = blt_afh_alarm(clk, f, tab);
    check("at the instant: the table, map 0 and the table-select bits written together, the switch over",
          r == 18u && (f->miscntl & 0xC0u) == 0x40u && (f->format & 0x40u) && tab[FM1_BR_OFF_FRQ + 80u] == 0x22 &&
          map_writes == 1u && map_words[2] == 9u && blt.afh_state == BLT_AFH_NONE && blt.afh_switches == 1u);
}

static void sig_before_media(void)   /* a published PDU goes before the stream; an unpublished one waits behind it */
{
    struct fm1_br_frame *f = (struct fm1_br_frame *)(void *)(tab + FM1_BR_OFF_FRAME);
    uint8_t pdu[3] = {0x22, 1, 2};
    memset(tab, 0, sizeof tab);
    memset(&blt, 0, sizeof blt);
    memset(&mq, 0, sizeof mq);
    f->txbufcntl[0] = f->txbufcntl[1] = 1;
    mq.n[0] = mq.n[1] = mq.n[2] = 332u, mq.w = 3u;
    (void)blt_put(pdu, 3u, 3u);
    blt_publish();
    blt_fill(f, tab);
    check("a published PDU and the stream: DM1 first, a DH5 in the other buffer", blt.r == 1u && mq.r == 1u &&
          (f->linkcntl[0] & 15u) == 3u && (f->linkcntl[1] & 15u) == 15u);
    blt_fill(f, tab);
    check("both buffers held by the engine: nothing more, counted busy", mq.r == 1u && mq.busy == 1u);
    f->txbufcntl[0] |= 1u;
    (void)blt_put(pdu, 3u, 3u);
    blt_fill(f, tab);
    check("queued but not published (the handler still running): the stream goes, the PDU waits",
          blt.r == 1u && mq.r == 2u);
    f->txbufcntl[0] |= 1u;
    blt_publish();
    blt_fill(f, tab);
    check("then published: it goes before the next audio packet", blt.r == 2u && mq.r == 2u);
}

int main(void)
{
    static const uint32_t loads[] = {50u, 100u, 130u, 200u};
    char what[200];
    uint32_t k, old_apart = 0;
    for (k = 0; k < sizeof loads / sizeof loads[0]; k++) {
        struct run R = simulate(loads[k], 0);
        printf("CPU0 %3u %%: AFH %u sent, %u on time, %u apart; silences %u; longest gap %u slots; a PDU waited at most "
               "%u slots; packets %u made, %u sent; %u signalling PDUs\n", (unsigned)loads[k], (unsigned)R.afh_sent,
               (unsigned)R.afh_on_time, (unsigned)R.afh_apart, (unsigned)R.silences, (unsigned)R.max_gap,
               (unsigned)R.sig_max_wait, (unsigned)R.pkts_made, (unsigned)R.pkts_sent, (unsigned)R.sig_sent);
        snprintf(what, sizeof what, "CPU0 at %u %%: every AFH switch together (instant on time), no 4 s silence: the link kept",
                 (unsigned)loads[k]);
        check(what, R.afh_sent >= 19u && R.afh_on_time == R.afh_sent && !R.afh_apart && !R.silences && !R.afh_late_rx);
        snprintf(what, sizeof what, "CPU0 at %u %%: each published signalling PDU handed over within 36 slots (IRQ 40), "
                 "each instant set at the hand-over", (unsigned)loads[k]);
        check(what, R.sig_sent > 10u && R.sig_max_wait <= 36u && !R.instant_short);
        snprintf(what, sizeof what, "CPU0 at %u %%: the stream's packets go out (over 90 %% of those the render made)",
                 (unsigned)loads[k]);
        check(what, R.pkts_sent * 10u >= R.pkts_made * 9u);
    }
    for (k = 1; k < sizeof loads / sizeof loads[0]; k++) {
        struct run R = simulate(loads[k], 1);
        printf("old TX side, CPU0 %3u %%: AFH %u sent, %u on time, %u apart; silences %u\n", (unsigned)loads[k],
               (unsigned)R.afh_sent, (unsigned)R.afh_on_time, (unsigned)R.afh_apart, (unsigned)R.silences);
        old_apart += R.afh_apart;
    }
    check("the old TX side (instant fixed when queued, the main loop hands over): at 100 % and over some switches go apart",
          old_apart > 0u);
    switch_at_instant();
    sig_before_media();
    printf("%s\n", fails ? "FAIL" : "all ok");
    return fails ? 1 : 0;
}
