/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The kept SBC frames of Bluetooth audio's render jobs (bt/br/bt_keep.c: plat_bta_job*), the code as built:
 *   - the bytes: frames kept, encoded as jobs and flushed (sent in order), or encoded at the flush, or both, are the
 *     bytes the encoder alone gives for the same PCM, in the same order (a tone and a silence between, so the silence copy
 *     of bt_sbc.c is on the path too)
 *   - two callers: a job called while a frame is being encoded (the other core's, modelled by a call from inside the
 *     encoder, as both cores meet) does nothing and does not encode that frame again; the pending count leaves the frame
 *     in flight out; the frames are encoded one each, in order, once
 *   - a frame kept needs the keep switched on and a free slot; the PCM is copied at once (the caller's buffer is free)
 *   - the claim's lock: the two cores' flags (Peterson's) let one in at a time, and a core's own flag is released
 *   - the kept PCM is 4-aligned (bt_sbc.c reads it as words) */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IN_POOL
#define BA_KEEP 2u
#include "../../bt/br/bt_sbc.c"

static int fails;
static void check(const char *what, int ok)
{
    printf("bt_keep: %-100s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

/* what bt_acl.c gives bt_keep.c */
static struct bt_sbc_ctx ba_sbc;
static uint8_t sent[16][128];
static uint32_t sent_len[16], sent_n;
static void ba_sbc_out(const uint8_t *frame, uint32_t slen)
{
    if (sent_n < 16u)
        memcpy(sent[sent_n], frame, slen), sent_len[sent_n] = slen;
    sent_n++;
}
static uint32_t core;                               /* the core that calls (BA_CORE) */
static uint32_t encodes;                            /* calls of the encoder */
static uint32_t in_flight, max_in_flight, rival_calls, rival_did, rival_pending;
static void (*meanwhile)(void);                     /* what the other core does while a frame is being encoded */
static uint32_t test_encode(const int16_t *pcm, uint8_t *out)
{
    uint32_t n;
    encodes++;
    in_flight++;
    if (in_flight > max_in_flight)
        max_in_flight = in_flight;
    if (meanwhile) {
        void (*m)(void) = meanwhile;
        meanwhile = 0;                              /* (once) */
        m();
    }
    n = bt_sbc_encode(&ba_sbc, pcm, out);
    in_flight--;
    return n;
}
#define BA_CORE() core
#define BA_ENCODE(pcm, out) test_encode((pcm), (out))
#include "../../bt/br/bt_keep.c"

#define NF 4u
static int16_t pcm[NF][2 * BT_SBC_SAMPLES_PER_FRAME] __attribute__((aligned(4)));
static uint8_t want[NF][128];
static uint32_t want_len[NF];

static void fill(void)                              /* tone, tone, silence, tone: the silence after a tone is encoded as it is */
{
    uint32_t f, i;
    for (f = 0; f < NF; f++)
        for (i = 0; i < BT_SBC_SAMPLES_PER_FRAME; i++) {
            int16_t v = f == 2u ? 0 : (int16_t)(((i * 7u + f * 31u) & 63u) * 400 - 12000);
            pcm[f][2u * i] = v;
            pcm[f][2u * i + 1u] = (int16_t)(f == 2u ? 0 : -v / 2);
        }
}
static void fresh(void)                             /* the encoder and the kept state from scratch */
{
    bt_sbc_init(&ba_sbc, 44100, 46, 1);
    ba_keep_n = ba_keep_done = ba_keep_busy = 0;
    ba_keep_flag[0] = ba_keep_flag[1] = 0;
    ba_feed_enc = 0;
    sent_n = 0;
    encodes = in_flight = max_in_flight = 0;
    core = 0;
    meanwhile = 0;
}
static int sent_is_want(uint32_t n)
{
    uint32_t f;
    if (sent_n != n)
        return 0;
    for (f = 0; f < n; f++)
        if (sent_len[f] != want_len[f] || memcmp(sent[f], want[f], want_len[f]))
            return 0;
    return 1;
}

static void rival(void)                             /* the second core calls a job while the first one's frame is encoded */
{
    uint32_t c = core;
    core = 1;
    rival_calls++;
    rival_pending = ba_keep_pending();
    rival_did += (uint32_t)ba_keep_one();
    core = c;
}

int main(void)
{
    uint32_t f;
    fill();
    fresh();                                        /* the reference: the encoder alone */
    for (f = 0; f < NF; f++)
        want_len[f] = bt_sbc_encode(&ba_sbc, pcm[f], want[f]);

    check("the kept PCM is 4-aligned", ((uintptr_t)ba_keep_pcm & 3u) == 0 && ((uintptr_t)ba_keep_pcm[1] & 3u) == 0);

    fresh();                                        /* not on: nothing kept */
    check("keep off: a frame is not kept", !ba_keep_add(pcm[0]) && ba_keep_n == 0 && ba_keep_pending() == 0);
    check("  no job to run, none taken", !ba_keep_one() && encodes == 0);

    fresh();                                        /* kept, then flushed with no job: the feed encodes them */
    ba_keep_on = 1;
    check("keep on: two frames kept, the third not (BA_KEEP slots)", ba_keep_add(pcm[0]) && ba_keep_add(pcm[1]) && !ba_keep_add(pcm[2]) && ba_keep_n == 2u);
    memset(pcm[0], 0x55, sizeof pcm[0]);            /* (the caller's buffer is free: the PCM was copied) */
    memset(pcm[1], 0x33, sizeof pcm[1]);
    check("  pending 2", ba_keep_pending() == 2u);
    ba_keep_flush();
    check("flush with no job: the two frames' bytes, in order, as the encoder alone gives them", sent_is_want(2) && ba_feed_enc == 2u);
    check("  the kept state is clear", ba_keep_n == 0 && ba_keep_done == 0 && ba_keep_busy == 0 && ba_keep_pending() == 0);
    fill();

    fresh();                                        /* every frame a job */
    ba_keep_on = 1;
    ba_keep_add(pcm[0]);
    ba_keep_add(pcm[1]);
    check("two jobs: each encodes one (1, 1), a third finds none (0)", ba_keep_one() == 1 && ba_keep_pending() == 1u && ba_keep_one() == 1 && ba_keep_one() == 0);
    check("  pending 0, the frames not sent before the feed", ba_keep_pending() == 0 && sent_n == 0);
    ba_keep_flush();
    check("  the feed's flush sends the jobs' bytes, in order, and encodes none itself", sent_is_want(2) && ba_feed_enc == 0 && encodes == 2u);

    fresh();                                        /* a job and the feed's share, the silence in the middle */
    ba_keep_on = 1;
    ba_keep_add(pcm[0]);
    ba_keep_add(pcm[1]);
    (void)ba_keep_one();
    ba_keep_flush();
    ba_keep_add(pcm[2]);
    ba_keep_add(pcm[3]);
    (void)ba_keep_one();
    ba_keep_flush();
    check("a job and the feed's share, over two halves with a silence: all four frames' bytes as the encoder alone", sent_is_want(4) && ba_feed_enc == 2u);

    fresh();                                        /* two callers: the second core meets the first one's frame in flight */
    ba_keep_on = 1;
    ba_keep_add(pcm[0]);
    ba_keep_add(pcm[1]);
    rival_calls = rival_did = rival_pending = 0;
    meanwhile = rival;
    check("job on the first core: its frame is encoded", ba_keep_one() == 1);
    check("  the second core's call meanwhile did nothing", rival_calls == 1 && rival_did == 0);
    check("  its pending count left out the frame in flight (1 of 2)", rival_pending == 1u);
    check("  the first frame was encoded once, the encoder never ran twice at a time", encodes == 1u && max_in_flight == 1u);
    check("  the second core's job then takes the second frame", (core = 1, ba_keep_one() == 1) && encodes == 2u && max_in_flight == 1u);
    core = 0;
    ba_keep_flush();
    check("  both frames' bytes as the encoder alone, in order", sent_is_want(2) && ba_feed_enc == 0);

    fresh();                                        /* the lock: one core in, the other waits; a core's own flag is released */
    ba_keep_lock(0);
    check("lock: core 0 in, its flag set, the other's not", ba_keep_flag[0] == 1 && ba_keep_flag[1] == 0);
    ba_keep_unlock(0);
    ba_keep_lock(1);
    ba_keep_unlock(1);
    check("  both released", ba_keep_flag[0] == 0 && ba_keep_flag[1] == 0);

    printf(fails ? "BT KEEP TEST FAILED\n" : "bt keep test passed\n");
    return fails != 0;
}
