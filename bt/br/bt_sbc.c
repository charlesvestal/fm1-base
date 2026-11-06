/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The SBC encoder (bt_sbc.h): AOSP's, its files in bt/sbc/ as Android ships them (Apache-2.0, their own headers kept),
 * compiled here as one unit; this file only sets it as the A2DP stream is configured. */
#include "bt_sbc.h"
#ifndef __PI32V2__
#include <string.h>                                 /* (host tests; the firmware has its own memset) */
#endif
#include "sbc/sbc_analysis.c"
#include "sbc/sbc_dct.c"
#include "sbc/sbc_dct_coeffs.c"
#include "sbc/sbc_enc_bit_alloc_mono.c"
#include "sbc/sbc_enc_bit_alloc_ste.c"
#include "sbc/sbc_enc_coeffs.c"
/* the stream is always 8 subbands and two channels in stereo or joint stereo (bt_sbc_init): the encoder's calls of
 * the 4-subband filter and of the mono / dual channel bit allocation are its others, never taken, so they name the
 * ones taken and those two functions (with the 4-subband DCT) are not linked: 1.5 KB of flash, the same frames */
_Static_assert(BT_SBC_SUBBANDS == 8 && BT_SBC_CHANNELS == 2, "bt_sbc.c: the encoder's 4-subband and mono paths left out");
#define SbcAnalysisFilter4 SbcAnalysisFilter8
#define sbc_enc_bit_alloc_mono sbc_enc_bit_alloc_ste
#include "sbc/sbc_encoder.c"
#undef SbcAnalysisFilter4
#undef sbc_enc_bit_alloc_mono
#include "sbc/sbc_packing.c"

/* Silence (2026-11-01, on the FM-1: a stopped FM-1 streaming to headphones spent ~25 % of a 32-frame mix block
 * on each frame of zeros): a frame of 128 zero samples whose filter history is zero too (the frame before it was all
 * zeros: the analysis window is 80 samples a channel) encodes to the same bytes every time for the same settings.
 * The first such frame is encoded and kept; the next ones copy it, and the filter's state moves on as the encoder would
 * move it: its history is all zeros already (a frame writes 128 samples a channel into a history of 112: the frame of
 * zeros before rewrote all of it), the shift counter on by the frame's 16 blocks. Bit-identical (tests/bt/bt_sbc_test.c:
 * a stream with silences of every length, at every phase of the shift counter, against the encoder alone) */
static struct {
    uint8_t prev_zero;                              /* the last frame was all zeros: the history is zero now */
    uint8_t len;                                    /* the kept frame's length (0: none) */
    int16_t bitpool, mode, freq;                    /* the settings it was encoded with */
    uint8_t frame[128];
} sbc_sil;
static int sbc_pcm_zero(const int16_t *pcm)
{
    const uint32_t *w = (const uint32_t *)(const void *)pcm;   /* (16-bit samples, 4-aligned: two a word) */
    uint32_t i, o = 0;
    for (i = 0; i < BT_SBC_SAMPLES_PER_FRAME; i++)  /* (two channels: 256 samples, 128 words) */
        o |= w[i];
    return o == 0u;
}
static void sbc_sil_advance(void)                   /* the analysis filter's state after a frame of zeros on zeros */
{
    uint32_t b;
    memset(s16X, 0, ENC_VX_BUFFER_SIZE * sizeof(int16_t));
    for (b = 0; b < BT_SBC_BLOCKS; b++)             /* (SbcAnalysisFilter8's step, a block of 8; SHIFTUP moves zeros) */
        ShiftCounter = ShiftCounter >= EncMaxShiftCounter ? 0 : (int16_t)(ShiftCounter + SUB_BANDS_8);
}

uint32_t bt_sbc_frame_len(uint8_t bitpool, uint8_t joint_stereo)
{   /* A2DP 1.3 12.9: 4 + (4 x subbands x channels) / 8 + ceil((join x subbands + blocks x bitpool) / 8), 2 channels */
    return 4u + 8u + ((joint_stereo ? 8u : 0u) + 16u * (uint32_t)bitpool + 7u) / 8u;
}

void bt_sbc_init(struct bt_sbc_ctx *ctx, uint32_t sample_rate, uint8_t bitpool, uint8_t joint_stereo)
{
    SBC_ENC_PARAMS *p = &ctx->p;
    uint32_t len;
    memset(p, 0, sizeof *p);
    if (bitpool < 2u || bitpool > 53u)
        bitpool = 53u;
    p->s16SamplingFreq = sample_rate == 48000u ? SBC_sf48000 : SBC_sf44100;
    p->s16ChannelMode = joint_stereo ? SBC_JOINT_STEREO : SBC_STEREO;
    p->s16NumOfSubBands = BT_SBC_SUBBANDS;
    p->s16NumOfChannels = BT_SBC_CHANNELS;
    p->s16NumOfBlocks = BT_SBC_BLOCKS;
    p->s16AllocationMethod = SBC_LOUDNESS;
    p->s16BitPool = bitpool;
    len = bt_sbc_frame_len(bitpool, joint_stereo);  /* (AOSP's init takes the bit rate and finds the bitpool from it) */
    p->u16BitRate = (uint16_t)((8u * len * sample_rate / 128u + 999u) / 1000u);
    p->Format = SBC_FORMAT_GENERAL;
    SBC_Encoder_Init(p);
    p->s16BitPool = bitpool;
    sbc_sil.prev_zero = 0, sbc_sil.len = 0;         /* (silence: from the next frames, encoded as they come) */                        /* (exactly the agreed one: the rate's rounding may not land on it) */
}

uint32_t bt_sbc_encode(struct bt_sbc_ctx *ctx, const int16_t *pcm, uint8_t *out)
{
    SBC_ENC_PARAMS *p = &ctx->p;
    uint32_t len, zero = (uint32_t)sbc_pcm_zero(pcm);
    if (zero && sbc_sil.prev_zero && sbc_sil.len && sbc_sil.bitpool == p->s16BitPool && sbc_sil.mode == p->s16ChannelMode &&
        sbc_sil.freq == p->s16SamplingFreq) {
        memcpy(out, sbc_sil.frame, sbc_sil.len);
        sbc_sil_advance();
        return sbc_sil.len;
    }
    len = SBC_Encode(p, (int16_t *)pcm, out);
    if (zero && sbc_sil.prev_zero && len <= sizeof sbc_sil.frame) {   /* (zeros on zeros: the frame to keep) */
        memcpy(sbc_sil.frame, out, len);
        sbc_sil.len = (uint8_t)len;
        sbc_sil.bitpool = p->s16BitPool, sbc_sil.mode = p->s16ChannelMode, sbc_sil.freq = p->s16SamplingFreq;
    }
    sbc_sil.prev_zero = (uint8_t)zero;
    return len;
}
