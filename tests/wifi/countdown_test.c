/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Henri David */
/* The Link example's start countdown (examples/link/countdown.h): the bar line and the beats left, as the session's beat
 * runs through a bar, on a simulated clock at 90 and 133 BPM and from every point of the bar. */
#include <stdio.h>
#include <stdint.h>
#include "../../examples/link/countdown.h"

#define Q 4000000
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    int64_t start, ub, target;
    int32_t mpb, left, last, steps;
    /* the bar line is the next multiple of the quantum, a whole bar away when asked on one */
    CHECK(cd_bar(9000000, 9000000 % Q, Q) == 12000000, "bar from 9 beats");
    CHECK(cd_bar(8000000, 0, Q) == 12000000, "a start asked on a bar line waits the whole bar");
    CHECK(cd_bar(8000001, 1, Q) == 12000000, "just after a bar line");
    CHECK(cd_bar(11999999, 3999999, Q) == 12000000, "just before one");
    /* the beats left, 4 .. 1 and then 0 */
    CHECK(cd_left(8000000, 12000000) == 4, "4 at the bar line");
    CHECK(cd_left(8000001, 12000000) == 4, "4 just after it");
    CHECK(cd_left(9000000, 12000000) == 3, "3");
    CHECK(cd_left(10500000, 12000000) == 2, "2 half a beat in");
    CHECK(cd_left(11999999, 12000000) == 1, "1 just before the next");
    CHECK(cd_left(12000000, 12000000) == 0, "0 on it");
    CHECK(cd_left(12000001, 12000000) == 0, "0 past it");
    CHECK(cd_left(-1, 12000000) > 4, "a beat before the session's start is more than a bar");
    /* a run: armed from every 10 ms of a bar, the loop asks every 10 ms (the session's beat advances at the tempo):
     * the count only falls, 4 .. 1, and starts within one loop step of the bar line, never before it */
    for (mpb = 450000; mpb <= 666667; mpb += 216667)       /* 133 and 90 BPM */
        for (start = 8000000; start < 12000000; start += 137777) {
            int64_t t, t_bar;
            target = cd_bar(start, start % Q, Q);
            last = 5;
            steps = 0;
            t_bar = (target - start) * mpb / 1000000;     /* us from arming to the bar line */
            for (t = 0;; t += 10000) {
                ub = start + t * 1000000 / mpb;
                left = cd_left(ub, target);
                CHECK(left <= last, "the count rose: %d after %d", (int)left, (int)last);
                CHECK(left <= 4, "more than 4 left: %d", (int)left);
                last = left;
                steps++;
                if (left == 0)
                    break;
                CHECK(t < t_bar, "not started at the bar line (mpb %d start %lld)", (int)mpb, (long long)start);
                if (t >= t_bar)
                    break;
            }
            CHECK(t >= t_bar && t - t_bar < 10000, "started %lld us after the bar line", (long long)(t - t_bar));
            CHECK(steps > 1, "a start with nothing to wait for");
        }
    if (!fails)
        printf("countdown: ok\n");
    return fails != 0;
}
