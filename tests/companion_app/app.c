/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Henri David */
/* The companion test app: see app.h */
static uint32_t cp_own;

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
void app_input_only(void) {}
void app_say(const char *a, const char *b) { (void)a, (void)b; }
int app_backup_playing(void) { return 0; }
void app_backup_flush(void) {}
void app_backup_hold(int on) { (void)on; }
void app_backup_restored(void) {}

int app_sysex(const uint8_t *p, uint32_t n)
{
    if (n < 3u || p[0] != 0x7Du || p[1] != 0x43u || p[2] != 0x50u)
        return 0;                                    /* (not the app's: the platform's, the companion's) */
    cp_own++;
    return 1;
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "own"))
        *got = (int)cp_own;
    else
        return 0;
    return 1;
}
#endif
