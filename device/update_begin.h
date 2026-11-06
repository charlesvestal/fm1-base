/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Henri David */
/* The app's stop before an update (APP_UPDATE_BEGIN, plat.h): asked once per main-loop pass until it says it is done,
 * or until PLAT_UPDATE_BEGIN_MS have passed since the first ask, whichever comes first. The update goes ahead then
 * whatever the hook says: the update and the soft key's UBOOT are the way out of a broken app, so they never wait for
 * the app longer than this. (A hook that does not return at all is not ended here; only the watchdog ends that.)
 * Host-testable: tests/update_begin_test.c. */
#ifndef UPDATE_BEGIN_H
#define UPDATE_BEGIN_H

#include <stdint.h>

#define PLAT_UPDATE_BEGIN_MS 300u                   /* the longest an update waits for the app's stop */

typedef struct {
    uint32_t t0;                                    /* the first ask's time */
    uint8_t on;                                     /* asking since t0 */
} update_begin_t;

/* one pass: 1 = go ahead with the update (the hook is done, there is none, or the time is up), 0 = ask again next
 * pass. hook: the app's app_update_begin (nonzero while it still needs time), or 0 */
static inline int update_begin_ready(update_begin_t *s, uint32_t now_ms, int (*hook)(void))
{
    if (!s->on) {
        s->on = 1;
        s->t0 = now_ms;
    }
    if (hook && (uint32_t)(now_ms - s->t0) < PLAT_UPDATE_BEGIN_MS && hook())
        return 0;
    s->on = 0;                                      /* (the next update asks afresh) */
    return 1;
}

#endif
