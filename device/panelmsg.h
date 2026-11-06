/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (C) 2026 Bitbazooka LLC (Les, les.audio)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice
 * and this permission notice shall be included in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * (Or, at your option, the Mozilla Public License 2.0, https://mozilla.org/MPL/2.0/.)
 *
 * Open panel messages for the FM-1, protocol 1: the scan side (module v3). The protocol is an open spec any app may
 * read or implement (the panel-messages spec); Les is one reader. Include BEFORE fm1_input.h: this file fills the HAL's two input
 * hooks (FM1_INPUT_EDGE, FM1_INPUT_STEP). Without it those hooks are empty and the HAL compiles exactly as before.
 *
 * While a computer is listening (panelmsg_on, set by panelmsg.c when a HELLO arrives) the scan ISR queues, IN THE ORDER
 * THEY HAPPENED, every debounced press and release of a button (ids 0..13) or key (14..40) and every encoder detent
 * (encoders 0..6). Detents of the same encoder in the same direction with nothing in between share one entry, so
 * press -> turn -> release stays in that order, a reversal stays visible, and a fast spin costs one entry, not forty.
 * Nobody listening: one byte read per edge or detent, nothing else. Nothing is ever stored past the next send.
 * Requirements (an integration must keep them): the hooks run in ONE interrupt on the same core as the main loop that
 * calls panelmsg_poll(), and that main loop's fm1__lock() masks that interrupt. */
#pragma once
#include <stdint.h>
#ifdef FM1_INPUT_EDGE
#error "panelmsg.h goes BEFORE fm1_input.h (the HAL's empty hook defaults are already defined here, so the module would hear nothing). A platform that reads the scan another way (fm1-base: device/panelmsg_base.c) #undefs the hooks first."
#endif
#define PANELMSG_ON 1                           /* panelmsg.c builds its half only with this one */

#define PANELMSG_RING 64u                            /* entries between two sends (a power of two) */
#define PANELMSG_NENC 7u
#define PANELMSG_K_PRESS 1u
#define PANELMSG_K_RELEASE 2u
#define PANELMSG_K_TURN 3u
#define PANELMSG_ENC_ID 48u                          /* a turn's id: 48 + encoder */

/* one entry: id (7 bits) | value (7 bits) << 7 | kind (2 bits) << 14. A turn's value is 64 + detents (1..127). */
static volatile uint8_t panelmsg_on;                 /* a computer said HELLO within the last PANELMSG_ALIVE_MS */
static volatile uint16_t panelmsg_ev[PANELMSG_RING];
static volatile uint32_t panelmsg_ev_w, panelmsg_ev_r;    /* producer: the scan ISR; consumer: the main loop, under fm1__lock */
static volatile uint32_t panelmsg_ev_seal;           /* entries before this one are sealed (a HELD snapshot follows them) */
static volatile uint16_t panelmsg_lost;              /* entries that found the ring full: always reported, never hidden */

static inline void panelmsg__put(uint32_t v)
{
    if (panelmsg_ev_w - panelmsg_ev_r < PANELMSG_RING) {
        panelmsg_ev[panelmsg_ev_w % PANELMSG_RING] = (uint16_t)v;
        panelmsg_ev_w++;
    } else if (panelmsg_lost < 0x3FFFu) {
        panelmsg_lost++;
    }
}
static inline void panelmsg_isr_edge(uint32_t id, uint32_t down)
{
    if (panelmsg_on)
        panelmsg__put((id & 0x7Fu) | (down ? PANELMSG_K_PRESS : PANELMSG_K_RELEASE) << 14);
}
static inline void panelmsg_isr_step(uint32_t e, int32_t n)
{
    uint32_t id = PANELMSG_ENC_ID + e;
    if (!panelmsg_on || e >= PANELMSG_NENC)
        return;
    while (n) {                                 /* (the HAL reports 1 or 2 detents at a time; any n is handled) */
        int32_t d = n > 63 ? 63 : n < -63 ? -63 : n;
        if (panelmsg_ev_w != panelmsg_ev_r && panelmsg_ev_w != panelmsg_ev_seal && !panelmsg_lost) {   /* newest entry unsent, unsealed, nothing dropped since */
            uint32_t last = panelmsg_ev[(panelmsg_ev_w - 1u) % PANELMSG_RING];
            int32_t was = (int32_t)((last >> 7) & 0x7Fu) - 64, v = was + d;
            if ((last >> 14) == PANELMSG_K_TURN && (last & 0x7Fu) == id && (was > 0) == (d > 0) && v >= -63 && v <= 63) {
                v += 64;                        /* same encoder, same direction: one entry (a reversal is a new one) */
                panelmsg_ev[(panelmsg_ev_w - 1u) % PANELMSG_RING] = (uint16_t)(id | (uint32_t)v << 7 | PANELMSG_K_TURN << 14);
                n -= d;
                continue;
            }
        }
        panelmsg__put(id | (uint32_t)(64 + d) << 7 | PANELMSG_K_TURN << 14);
        n -= d;
    }
}

#define FM1_INPUT_EDGE(id, down) panelmsg_isr_edge((id), (down))
#define FM1_INPUT_STEP(e, n) panelmsg_isr_step((e), (n))
#define FM1_INPUT_HOOKED 1                      /* a HAL that must test for a change first does so only if hooked */
