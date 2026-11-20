/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Henri David */
/* The real-time MIDI test app: see app.h */
static uint32_t fast_ticks, rt_sent;               /* app_tick_fast calls; clock ticks plat_midi_rt took */

void app_init(void) {}
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i;
    (void)master_q12;
    for (i = 0; i < 2u * n; i++)
        lr[i] = 0;
}
void app_panic(void) {}
void app_ui_init(void) { lcd_fill(0, 0, 240, 240, C_BLACK); }
void app_frame(void) {}
void app_tick_fast(void)                           /* 10 kHz: every tenth call, a clock tick (0xF8, cable 0) */
{
    if (++fast_ticks % 10u == 0u)
        rt_sent += (uint32_t)(plat_midi_rt(0x0Fu | 0xF8u << 8) != 0);
}
void app_input_only(void) {}
void app_say(const char *a, const char *b) { (void)a, (void)b; }
#ifdef PLAT_HOST                                   /* (the simulator's checks: not in the device build) */
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "app_fast_ticks"))
        *got = (int)fast_ticks;
    else if (!strcmp(what, "app_rt_sent"))
        *got = (int)rt_sent;
    else
        return 0;
    return 1;
}
#endif
