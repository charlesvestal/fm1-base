/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* BA_KEEP (bt_own_audio.c, plat_bta_job*: 2026-11-01, an app with a second core): the frames a half completes are kept,
 * their PCM copied, and encoded during the next render, one an app job (on either core), instead of inline on CPU0 after
 * the render (two a 256-frame half, ~4x a synth part's per-sample cost each). Sent at the next feed, in order, the same
 * bytes, one half (5.8 ms) later. Not taken (keep off, CPU1 down and no job ran, the slots full): encoded at the feed as
 * before. Included by bt_acl.c (the kept frames' state and its calls); host-tested: tests/bt/bt_keep_test.c.
 *
 * One frame is encoded at a time: the encoder's state runs from one frame to the next, and a frame being encoded is
 * "in flight" (ba_keep_busy) from its claim to its end. A job (plat_bta_job) that finds one in flight, or none left,
 * does nothing and says so (0); the pending count (plat_bta_job_pending) leaves the one in flight out. The claim is made
 * under a lock of the two cores (Peterson's: the chip has no atomic read-modify-write), taken from the render's interrupt
 * on the first core and from its job on the second, never from the main loop (the same core's interrupt would find its
 * own flag set). The lock's words (the two flags, the turn) and the claim's (busy, done) are a 32-bit word each, alone
 * on a line of 32 bytes (BA_LINE: the alignment felucca-bsp's fm1_cpu1.h gives its mailbox, "a cache line of their
 * own"), not bytes packed into one block. A flag has the one writer that is its core (felucca-bsp's rule, fm1_cpu1.h: a
 * single writer for each word); turn has two by Peterson's design, busy and done have the core that holds the claim, one
 * at a time. The lock has not run on two real cores yet.
 * Needs from the includer: IN_POOL, bt_sbc_encode and ba_sbc, ba_sbc_out(frame, len); BA_CORE() (0 or 1: the calling core)
 * and BA_SYNC() (a barrier between the cores' reads and writes) where there are two cores */
#ifndef BA_CORE
#define BA_CORE() 0u
#endif
#ifndef BA_SYNC
#define BA_SYNC() __asm__ volatile("" ::: "memory")
#endif
#ifndef BA_LINE
#define BA_LINE 32u                                 /* the data cache line, bytes (felucca-bsp fm1_cpu1.h: its mailbox words) */
#endif
#ifndef BA_ENCODE
#define BA_ENCODE(pcm, out) bt_sbc_encode(&ba_sbc, (pcm), (out))
#endif
static int16_t ba_keep_pcm[BA_KEEP][BT_SBC_SAMPLES_PER_FRAME * 2] IN_POOL __attribute__((aligned(4)));   /* (sbc_pcm_zero reads words) */
static uint8_t ba_keep_frm[BA_KEEP][128] IN_POOL;
static uint32_t ba_keep_len[BA_KEEP];
typedef struct { volatile uint32_t v; } __attribute__((aligned(BA_LINE))) ba_word;   /* a word alone on a cache line */
_Static_assert(sizeof(ba_word) == BA_LINE, "ba_word fills a cache line");
static volatile uint8_t ba_keep_n;                 /* frames kept (the first core's) */
static ba_word ba_keep_done;                       /* encoded of them (in order) */
static ba_word ba_keep_busy;                       /* one is being encoded: frame ba_keep_done, claimed, not done */
static volatile uint8_t ba_keep_on;                /* the app runs the jobs (plat_bta_job_keep) */
static ba_word ba_keep_flag[2], ba_keep_turn;      /* the claim's lock: a flag a core (its own writer) and whose turn it is to wait */
static uint32_t ba_feed_enc;                       /* frames encoded here at the feed, on CPU0 after the render (not a job;
                                                    * plat_bta_job_inline: the app's CPU TIMES) */

static void ba_keep_lock(uint32_t c)
{
    uint32_t o = c ^ 1u;
    ba_keep_flag[c].v = 1u;
    ba_keep_turn.v = o;
    BA_SYNC();
    while (ba_keep_flag[o].v && ba_keep_turn.v == o)
        BA_SYNC();
}
static void ba_keep_unlock(uint32_t c)
{
    BA_SYNC();
    ba_keep_flag[c].v = 0u;
}

static int ba_keep_add(const int16_t *pcm)         /* a half's frame kept for the render's jobs (its PCM copied): 1, 0 not kept */
{
    uint32_t k;
    if (!ba_keep_on || ba_keep_n >= BA_KEEP)
        return 0;
    for (k = 0; k < 2u * BT_SBC_SAMPLES_PER_FRAME; k++)
        ba_keep_pcm[ba_keep_n][k] = pcm[k];
    BA_SYNC();                                     /* (the PCM before the count that lets a job read it) */
    ba_keep_n++;
    return 1;
}

static uint32_t ba_keep_cnt(uint32_t n)            /* a count read from the shared RAM, kept within the slots (some units misread it) */
{
    return n > BA_KEEP ? BA_KEEP : n;
}

static int ba_keep_one(void)                       /* the next kept frame encoded (a job: either core; or the feed): 1; 0 when
                                                    * none is left or one is in flight (nothing done) */
{
    uint32_t c = BA_CORE() & 1u, k = 0, d;
    ba_keep_lock(c);
    d = ba_keep_done.v;
    if (!ba_keep_busy.v && d < ba_keep_cnt(ba_keep_n)) {   /* (the index bound: d < BA_KEEP) */
        ba_keep_busy.v = 1u;
        k = d + 1u;
    }
    ba_keep_unlock(c);
    if (!k)
        return 0;
    k--;
    ba_keep_len[k] = BA_ENCODE(ba_keep_pcm[k], ba_keep_frm[k]);
    BA_SYNC();                                     /* (the frame before the count that says it is done) */
    ba_keep_done.v = k + 1u;
    BA_SYNC();
    ba_keep_busy.v = 0u;
    return 1;
}

static uint32_t ba_keep_pending(void)              /* kept frames not yet taken: the one being encoded is not counted */
{
    uint32_t b = ba_keep_busy.v, d = ba_keep_done.v, n = ba_keep_cnt(ba_keep_n);
    return n > d + b ? n - d - b : 0u;
}

static void ba_sbc_out(const uint8_t *frame, uint32_t slen);
static void ba_keep_flush(void)                    /* every kept frame encoded (the rest here) and sent, in order */
{
    uint32_t k, n = ba_keep_cnt(ba_keep_n);
    while (ba_keep_done.v < n)                     /* (one in flight on the other core: waited for, its frame is next) */
        if (ba_keep_one())
            ba_feed_enc++;
    for (k = 0; k < n; k++)
        ba_sbc_out(ba_keep_frm[k], ba_keep_len[k]);
    ba_keep_n = 0;
    ba_keep_done.v = 0u;
}
