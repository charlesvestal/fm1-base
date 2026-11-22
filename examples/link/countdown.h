/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Henri David */
/* The Link example's start countdown, as arithmetic on the session's beat (micro-beats) and nothing else, so that
 * tests/wifi/countdown_test.c checks it without the platform. PLAY while a session has peers does not start at once:
 * it waits for the session's next bar (cd_bar), shows the beats left (cd_left, 4 .. 1) and starts when the beat reaches
 * it (cd_left 0). */
#pragma once
#include <stdint.h>

#define CD_BEAT 1000000                          /* one beat, in micro-beats */

/* the next bar line after the beat ub, where phase is ub modulo the quantum q (plat_link_phase(ub, q)): a start asked
 * on a bar line waits the whole bar */
static inline int64_t cd_bar(int64_t ub, int64_t phase, int64_t q) { return ub - phase + q; }

/* the beats left from ub to the bar line at target, counting a started one: 4 .. 1, then 0 once ub has reached it */
static inline int32_t cd_left(int64_t ub, int64_t target)
{
    return ub >= target ? 0 : (int32_t)((target - ub + CD_BEAT - 1) / CD_BEAT);
}
