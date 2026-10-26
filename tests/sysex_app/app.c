/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Henri David */
/* The SysEx test app: see app.h */
static uint32_t sx_frames, sx_other;
static uint8_t sx_reply[700];

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

int app_sysex(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    if (n < 4u || p[0] != 0x7Du || p[1] != 0x53u || p[2] != 0x58u) {
        sx_other++;
        return 0;
    }
    sx_frames++;
    if (p[3] == 1u && n + 2u <= sizeof sx_reply) {   /* echo */
        sx_reply[0] = 0xF0;
        for (i = 0; i < n; i++)
            sx_reply[1u + i] = p[i];
        sx_reply[1u + n] = 0xF7;
        plat_sysex_send(sx_reply, n + 2u);
    }
    return 1;
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "frames"))
        *got = (int)sx_frames;
    else if (!strcmp(what, "other"))
        *got = (int)sx_other;
    else
        return 0;
    return 1;
}
#endif
