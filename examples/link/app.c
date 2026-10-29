/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Henri David */
/* The Link example: Wi-Fi (MODE AUTO: the networks kept are joined; a new one is given over USB, tools/fm1_remote.py
 * or the "LW" SysEx of device/main_fm1.c) and Ableton Link. On the screen: the Wi-Fi state and address, the session's
 * tempo, its peers, the beat in the bar (a quantum of 4) and whether it plays. A click on each beat, louder on the
 * bar's first: its time is the session's (plat_link_time_at), given to the tick as a plat_ticks24 value, not the
 * arrival of anything. PLAY starts and stops the session; K1 turns its tempo; SEL switches Link on and off.
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c is in scope. */
#define Q_UB 4000000                             /* the quantum: 4 beats, in micro-beats */

static struct {
    uint8_t link_on;
    uint32_t btn;
    uint8_t redraw;
    int64_t next_ub;                             /* the next beat to click, micro-beats */
    uint32_t beats;                              /* clicks given (the host's checks) */
} ui = {1, 0, 1, -1, 0};

/* ---- the click: the main loop sets its time, the tick starts it, the render plays it */
static volatile uint32_t click_t24, click_due, click_accent;
static volatile uint32_t click_n;                /* samples of the click left (the render) */
static volatile uint32_t click_amp;
void app_tick(void)
{
    if (click_due && (int32_t)(plat_ticks24() - click_t24) >= 0) {
        click_due = 0;
        click_amp = click_accent ? 3000000u : 1500000u;
        click_n = 441u;                          /* 10 ms */
        ui.beats++;
    }
}
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t s = 0;
        if (click_n) {
            click_n--;
            s = (int32_t)((((click_n / 25u) & 1u) ? click_amp : 0u - click_amp) >> 2) * (int32_t)(master_q12 >> 4) / 64;
        }
        lr[2 * i] = lr[2 * i + 1] = s;
    }
}
void app_panic(void) { click_n = 0; click_due = 0; }

static void schedule(void)                       /* the next beat's time to the tick, 50 ms ahead at most */
{
    int64_t now = plat_link_now(), ub, t;
    if (!plat_link_on() || click_due) {
        if (!plat_link_on())
            ui.next_ub = -1;
        return;
    }
    ub = plat_link_beat_at(now);
    if (ub < 0)
        return;
    if (ui.next_ub < 0 || ui.next_ub <= ub - 1000000)
        ui.next_ub = ub - plat_link_phase(ub, 1000000) + 1000000;   /* the next whole beat */
    t = plat_link_time_at(ui.next_ub);
    if (t - now > 50000)
        return;
    click_accent = plat_link_phase(ui.next_ub, Q_UB) == 0;
    click_t24 = plat_link_t24(t);
    click_due = 1;
    ui.next_ub += 1000000;
}

void app_init(void) {}

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}
static void num_text(char *b, int32_t v)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    while (*b)
        b++;
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}
void app_say(const char *a, const char *b) { (void)a, (void)b; }

static void draw(void)
{
    static const char *const ST[] = {"OFF", "STARTING", "ON", "JOINING", "ADDRESS", "CONNECTED", "FAILED"};
    char t[40] = "";
    uint32_t ip = plat_wifi_addr(), i;
    int32_t mpb = plat_link_mpb();
    int64_t ub = plat_link_on() ? plat_link_beat_at(plat_link_now()) : -1;
    cv_begin(240, 120, C_BLACK);                 /* (two bands: the canvas holds 240 x 144) */
    cv_text(10, 6, &FONT_B, APP_NAME, C_WHITE);
    cv_text(10, 40, &FONT_XS, "WI-FI", C_GRAY);
    cv_text(60, 40, &FONT_XS, ST[plat_wifi_state() % 7], plat_wifi_state() == WIFI_CONNECTED ? C_HI : C_WHITE);
    cv_text(60, 54, &FONT_XS, plat_wifi_ssid(), C_WHITE);
    if (ip) {
        for (i = 0; i < 4u; i++) {
            num_text(t, (int32_t)(ip >> (24u - 8u * i) & 255u));
            if (i < 3u)
                text_cat(t, ".");
        }
        cv_text(60, 68, &FONT_XS, t, C_WHITE);
    } else if (plat_wifi_why()[0]) {
        cv_text(60, 68, &FONT_XS, plat_wifi_why(), C_DIM);
    }
    cv_text(10, 100, &FONT_XS, "LINK", C_GRAY);
    cv_text(60, 100, &FONT_XS, !ui.link_on ? "OFF" : plat_link_on() ? "ON" : "WAITING", plat_link_on() ? C_HI : C_WHITE);
    t[0] = 0;
    num_text(t, (int32_t)plat_link_peers());
    text_cat(t, plat_link_peers() == 1u ? " PEER" : " PEERS");
    cv_text(130, 100, &FONT_XS, t, C_WHITE);
    t[0] = 0;
    if (mpb > 0) {
        num_text(t, 60000000 / mpb);
        text_cat(t, " BPM");
    }
    cv_blit(0, 0);
    cv_begin(240, 120, C_BLACK);
    cv_text(60, 4, &FONT_B, t, C_WHITE);
    for (i = 0; i < 4u; i++) {                   /* the beat in the bar */
        int lit = ub >= 0 && (int32_t)plat_link_phase(ub, Q_UB) / 1000000 == (int32_t)i;
        cv_rect(60 + (int32_t)i * 40, 40, 32, 24, lit ? C_HI : C_LINE);
    }
    cv_text(60, 76, &FONT_XS, plat_link_playing() ? "PLAYING" : "STOPPED", plat_link_playing() ? C_HI : C_DIM);
    cv_blit(0, 120);
}

static void input(void)
{
    uint32_t btn = plat_buttons(), down = btn & ~ui.btn;
    int e;
    ui.btn = btn;
    if (down >> B_PLAY & 1u)
        plat_link_set_playing(!plat_link_playing());
    if (down >> B_SEL & 1u) {
        ui.link_on = (uint8_t)!ui.link_on;
        plat_link_enable(ui.link_on);
    }
    if ((e = plat_enc(EN_K1)) != 0 && plat_link_mpb() > 0) {
        int32_t bpm = 60000000 / plat_link_mpb() + e;
        plat_link_set_tempo(60000000 / (bpm < 20 ? 20 : bpm > 999 ? 999 : bpm));
    }
    plat_leds(btn | (plat_link_playing() ? 1u << B_PLAY : 0u) | (ui.link_on ? 1u << B_SEL : 0u), 0);
}
void app_input_only(void) { input(); }

void app_ui_init(void)
{
    plat_wifi_on(1);                             /* MODE AUTO, unless the mode kept says otherwise */
    plat_link_enable(ui.link_on);
    lcd_fill(0, 0, 240, 240, C_BLACK);
}
void app_frame(void)
{
    static uint32_t last;
    input();
    schedule();
    if (plat_ms() - last >= 100u) {              /* the screen ten times a second */
        last = plat_ms();
        draw();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "beats"))
        *got = (int)ui.beats;
    else
        return 0;
    return 1;
}
#endif
