/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* fm1-base on the SMK-37 (Pro / Elite / MKE-P37; PLAT_SMK37): what the board adds to plat.h over felucca-bsp's
 * boards/smk37/ layer, and the SMK's own versions of the FM-1's lights, MASTER and battery. Part of the unity build:
 * included by plat_fm1.c (after its common part), before main_fm1.c. Not yet confirmed on a unit (it follows Melodee's
 * own SMK-37 port, which runs on an Elite).
 *
 * MASTER: the FM-1's pot is K5, an encoder, here: a virtual master, 0..4096 in steps of SMK_MASTER_STEP a detent,
 * kept in the panel calibration object (OBJ_PANEL, saved SMK_MASTER_SAVE_MS after the last turn), so it survives
 * power-off; plat_master_set lets an app put its own saved level there at start.
 * Lights: the 18 buttons' LEDs by the panel map, PLAY's second colour on the spare line (LED_PLAY_GREEN); no key
 * LEDs (the keys parameters are ignored). The glow, mid and breath levels are the board's column counts.
 * Pads: 16 RGB LEDs (WS2812 class) on the LCD's SPI, one frame between two LCD transfers (smk_pads_flush, from the
 * main loop after each UI frame) when a colour changed; plat_pad_rgb sets a colour, shown at the next frame.
 * The TRS jack: MIDI OUT (midi_trs_out.c).
 * The store's owner (boards/smk37/fm1_flash.h FL_OWNER): checked at every boot, before the store is read. */

#define SMK_MASTER_STEP 96                         /* a detent of K5 */
#define SMK_MASTER_SAVE_MS 2000u
#define SMK_MASTER_ENC 4u                          /* K5 (0-based) */
_Static_assert(FM1_TRIM_BASE == FL_USRTRIM, "the input layer reads the calibration record where the flash map puts it");
_Static_assert(FL_DATA_LO == PLAT_DATA_LO && FL_DATA_HI == PLAT_DATA_HI, "plat.h's data window is the SMK's");

uint64_t plat_keys37(void)                         /* (the remote's and the mirror's 27 land in the middle) */
{
    uint64_t k = (uint64_t)fm1_in.keys37_lo | (uint64_t)fm1_in.keys37_hi << 32;
    uint64_t r = (uint64_t)((remote.keys | mirror_in_keys()) & ((1u << NKEYS) - 1u)) << FM1_KEY_FM1_FIRST;
    return k | r;
}
uint64_t plat_keys37_pressed(void)
{
    uint32_t lo, hi;
    fm1_input_key37_edges(&lo, &hi);
    return (uint64_t)lo | (uint64_t)hi << 32;
}
int plat_key_vel(int k) { return k >= 0 && k < (int)FM1_NKEY37 ? fm1_in.key_vel[k] : 0; }
uint32_t plat_pads(void) { return fm1_in.pads & 0xFFFFu; }
uint32_t plat_pads_pressed(void) { return fm1_input_pad_edges() & 0xFFFFu; }
int plat_pad_vel(int p) { return p >= 0 && p < (int)FM1_NPAD ? fm1_in.pad_vel[p] : 0; }
int plat_pad_press(int p) { return p >= 0 && p < (int)FM1_NPAD ? fm1_in.pad_press[p] : 0; }
int plat_fader(int i) { return i >= 0 && i < 4 ? fm1_in.fader[i] : 0; }
#include "smk37_analog.h"   /* the wheels and the pedal: plat_wheel, plat_pedal (smk_analog_poll, from the main loop) */
uint64_t plat_keys37_retrig(void)
{
    uint32_t lo, hi;
    fm1_input_key37_retrig(&lo, &hi);
    return (uint64_t)lo | (uint64_t)hi << 32;
}
void plat_vel_curve(int mode) { fm1_vel_mode = (uint8_t)(mode & 3); }

/* the battery: mux C's divider, a slow average by the main loop; the stock firmware's thresholds (analysis: raw 530,
 * 560, 602 for its four levels; the divider is not measured). A USB host means power, as on the FM-1 */
static int32_t batt_raw;
static int8_t batt_bars = -1;
int plat_battery(void)
{
    static const int16_t AT[3] = {530, 560, 602};
    if (usb.config && !usb.suspended)
        return 4;
    if (!batt_raw)
        return -1;
    if (batt_bars < 0)
        batt_bars = (int8_t)((batt_raw >= AT[0]) + (batt_raw >= AT[1]) + (batt_raw >= AT[2]));
    while (batt_bars < 3 && batt_raw >= AT[batt_bars] + 4)
        batt_bars++;
    while (batt_bars > 0 && batt_raw < AT[batt_bars - 1] - 4)
        batt_bars--;
    return batt_bars;
}
#ifdef APP_BATTERY_RAW
int32_t plat_battery_raw(void) { return batt_raw; }   /* (mux C's divider, averaged: the bars' reading) */
#endif
/* no headphone switch known on the SMK-37 (the FM-1's is PB5: plat_hp.c); tools/build.py refuses headphones = true */
#if FM1B_HEADPHONES
#error "fm1.toml headphones = true: the FM-1's jack switch; the SMK-37 has none known"
#endif
int plat_headphones(void) { return -1; }

/* MASTER: K5's detents on a virtual level, kept in the panel object */
static int32_t smk_master = 2896;                  /* half gain, until the panel object or the app says */
static uint32_t smk_master_t;                      /* the last turn (fm1_ms), 0: nothing to save */
void plat_master_set(uint32_t q12)
{
    smk_master = (int32_t)(q12 > 4096u ? 4096u : q12);
    master_pos = mirror_in_master(remote.master >= 0 ? (uint32_t)remote.master : (uint32_t)smk_master);
    smk_master_t = fm1_ms | 1u;                    /* kept like a turn of K5 (the save skips a level already there) */
}
static void smk_master_load(void)                  /* after panel_init: the level saved with the calibration */
{
    if (panel.master <= 4096u)
        smk_master = (int32_t)panel.master;
    master_pos = (uint32_t)smk_master;
}
static void smk_main_poll(void)                    /* the main loop: K5, the wheels, the pedal, the battery's average, the saved master */
{
    int32_t s = fm1_enc_take(SMK_MASTER_ENC);
    smk_analog_poll();
    if (s) {
        smk_master = clamp(smk_master + s * SMK_MASTER_STEP, 0, 4096);
        smk_master_t = fm1_ms | 1u;
    }
    master_pos = mirror_in_master(remote.master >= 0 ? (uint32_t)remote.master : (uint32_t)smk_master);
    if (smk_master_t && fm1_ms - smk_master_t >= SMK_MASTER_SAVE_MS) {
        smk_master_t = 0;
        if (panel.master != (uint32_t)smk_master) {
            panel.master = (uint32_t)smk_master;
            plat_store_save(OBJ_PANEL, &panel, sizeof panel);
        }
    }
    if (fm1_in.battery)
        batt_raw = batt_raw ? batt_raw + ((int32_t)fm1_in.battery - batt_raw) / 16 : (int32_t)fm1_in.battery;
}
static void smk_input_start(void)                  /* after fm1_input_init and fm1_adc_init: the analog slots run */
{
    fm1_an.con = (uint16_t)FM1_ADC_CON(5, 4);      /* the stock timing: ~40 us, well inside a tick */
}

/* Who owns the store. Every firmware ported to these keyboards shares the same flash, each with its own formats, so
 * none may parse another's data: sector FL_OWNER holds "SMK37DATA" + the firmware's family. Ours: as ever. Anything else
 * (another firmware's, the stock firmware's, erased): this firmware's windows are cleared and the sector tagged last, so
 * a power loss clears them again. The stock firmware's end marker goes too, so a return to stock rebuilds its mapping.
 * BTIF, USRTRIM, USR and key_mac are never touched (the flash driver refuses them). */
#ifndef APP_DATA_FAMILY
#define APP_DATA_FAMILY "FM1BASE"
#endif
_Static_assert(sizeof APP_DATA_FAMILY - 1u >= 1u && sizeof APP_DATA_FAMILY - 1u <= 7u, "APP_DATA_FAMILY: 1..7 characters");
static int smk_clear_window(uint32_t lo, uint32_t hi)
{
    uint32_t s, took;
    int bad = 0;
    for (s = lo; s < hi; s += FL_SECTOR) {
        if (s != FL_OWNER)
            bad |= fl_erase4k(s, &took) != 0;
        fm1_wdt_feed();
    }
    return bad;
}
static void smk_store_owner(void)
{
    static const char FAM[] = APP_DATA_FAMILY;
    uint8_t want[16];
    const uint8_t *tag = fm1_xip_ptr(FL_OWNER);
    uint32_t i, took, ours = 1;
    int bad;
    for (i = 0; i < 16u; i++)
        want[i] = i < 9u ? (uint8_t)"SMK37DATA"[i] : i - 9u < sizeof FAM - 1u ? (uint8_t)FAM[i - 9u] : 0u;
    for (i = 0; i < 16u; i++)
        ours &= tag[i] == want[i];
    if (ours)
        return;
    audio_silence();
    bad = fl_erase4k(FL_STOCK_MARK, &took) != 0;
    bad |= smk_clear_window(FL_XDATA0_LO, FL_XDATA0_HI);
    bad |= smk_clear_window(FL_AUTO_LO, FL_AUTO_HI);
    bad |= smk_clear_window(FL_XDATA1_LO, FL_XDATA1_HI);
    bad |= smk_clear_window(FL_DATA_LO, FL_DATA_HI);
    bad |= smk_clear_window(FL_GLOB_LO, FL_GLOB_HI);
    if (!bad && !fl_erase4k(FL_OWNER, &took))
        (void)fl_write(FL_OWNER, want, sizeof want);
}

/* LEDs: the 18 buttons' by the panel map, PLAY's second colour on the spare line; the pictures are the board's four
 * (lit, the glow, mid, breath), rebuilt whole on each call. The dim ones are the board's column counts, two levels
 * (fm1_led_dim_level), not a pulse width: no plat_dim_ns (APP_DIM_LEVELS) here yet */
#ifdef APP_DIM_LEVELS
#error "APP_DIM_LEVELS (plat_dim_ns): the SMK-37's LEDs dim by columns, two levels; not there yet"
#endif
#define SMK_MIRROR_B ((1u << (B_OCTUP + 1)) - 1u)  /* the mirror's buttons: the FM-1's 14 (bit 15 is PLAY red there) */
static void led_pos_init(void) {}
static void smk_leds_build(uint16_t *grp, uint32_t buttons, uint32_t keys)   /* buttons: B_* bits + LED_PLAY_GREEN */
{
    uint32_t i;
    (void)keys;                                    /* (no key LEDs on the SMK) */
    grp[0] = grp[1] = 0;
    for (i = 0; i < NB; i++)                       /* PLAY's own LED is its green; B_PLAY asks for the other colour */
        if (i != B_PLAY && ((buttons >> i) & 1u))
            fm1_led_set(grp, panel.btn[i], 1);
    if (buttons & (1u << B_PLAY))
        fm1_led_set(grp, FM1_LED_PLAY2, 1);
    if (buttons & LED_PLAY_GREEN)
        fm1_led_set(grp, panel.btn[B_PLAY], 1);
}
void plat_lights(uint32_t buttons, uint32_t keys, uint32_t dim_buttons, uint32_t dim_keys, int glow)
{
    static int level = -1;
    uint16_t nl[2], dl[2];
    if (glow > 0 && glow != level) {
        fm1_led_dim_level(glow == 1 ? 1u : 0u);    /* 1: DIM LO, 2: DIM HI */
        level = glow;
    }
    smk_leds_build(nl, buttons, keys);
    smk_leds_build(dl, glow > 0 ? dim_buttons & ~buttons : 0u, dim_keys);
    fm1_led[0] = nl[0];
    fm1_led[1] = nl[1];
    fm1_led_dim[0] = (uint16_t)(dl[0] & ~nl[0]);
    fm1_led_dim[1] = (uint16_t)(dl[1] & ~nl[1]);
    {   /* the mirror's view: PLAY lit (either LED), and bit 15 PLAY lit red; the dim ones the same way */
        uint32_t play = 1u << B_PLAY, lit = buttons & SMK_MIRROR_B;
        uint32_t db = glow > 0 ? dim_buttons : 0u, dk = glow > 0 ? dim_keys : 0u;
        mirror_leds((lit & ~play) | (buttons & (play | LED_PLAY_GREEN) ? play : 0u) | (buttons & play ? 1u << 15 : 0u),
                    keys, (db & SMK_MIRROR_B & ~play) | (db & (play | LED_PLAY_GREEN) ? play : 0u), dk);
    }
}
void plat_lights_mid(uint32_t buttons, uint32_t keys)
{
    uint16_t ml[2];
    smk_leds_build(ml, buttons, keys);
    fm1_led_mid[0] = (uint16_t)(ml[0] & ~fm1_led[0]);
    fm1_led_mid[1] = (uint16_t)(ml[1] & ~fm1_led[1]);
    mirror_mid(buttons & SMK_MIRROR_B, keys);
}
static uint8_t glow_on = 1, play_red;
void plat_glow(int on) { glow_on = (uint8_t)(on != 0); }
void plat_play_red(int red) { play_red = (uint8_t)(red != 0); }
void plat_leds(uint32_t buttons, uint32_t keys)    /* PLAY green, unless red (recording); the unlit buttons glow */
{
    uint32_t play = 1u << B_PLAY, b = buttons, dim = glow_on ? ((1u << NB) - 1u) & ~buttons : 0u;
    if (!play_red) {
        b = (b & ~play) | (buttons & play ? LED_PLAY_GREEN : 0u);
        dim = (dim & ~play) | (dim & play ? LED_PLAY_GREEN : 0u);
    }
    plat_lights(b, keys, dim, 0, glow_on ? 2 : 0);
}

/* the pad LEDs: colours in, one WS2812 frame out between LCD transfers (boards/smk37/fm1_lcd_hw.h) */
static uint8_t smk_pad_rgb[NPADS][3];              /* G, R, B as the LEDs take them */
static uint8_t smk_pad_dirty = 1;                  /* (the first frame: all dark) */
static uint8_t smk_padbuf[FM1_PADLED_FRAME] PLAT_POOL __attribute__((aligned(4)));
static const uint8_t SMK_PAD_SLOT[NPADS] = {0, 15, 14, 13, 12, 11, 10, 9, 1, 2, 3, 4, 5, 6, 7, 8};   /* pad p's place in the
                                                                                                * LEDs' chain */
void plat_pad_rgb(int p, uint32_t rgb)
{
    uint8_t g = (uint8_t)(rgb >> 8), r = (uint8_t)(rgb >> 16), b = (uint8_t)rgb;
    if (p < 0 || p >= NPADS)
        return;
    if (smk_pad_rgb[p][0] == g && smk_pad_rgb[p][1] == r && smk_pad_rgb[p][2] == b)
        return;
    smk_pad_rgb[p][0] = g;
    smk_pad_rgb[p][1] = r;
    smk_pad_rgb[p][2] = b;
    smk_pad_dirty = 1;
}
static void smk_pads_flush(void)                   /* main loop, after a UI frame */
{
    uint32_t i, c, bit;
    if (!smk_pad_dirty)
        return;
    smk_pad_dirty = 0;
    for (i = 0; i < FM1_PADLED_RESET; i++)
        smk_padbuf[i] = 0;
    for (i = 0; i < NPADS; i++) {
        uint8_t *o = smk_padbuf + FM1_PADLED_RESET + SMK_PAD_SLOT[i] * 24u;
        for (c = 0; c < 3u; c++)
            for (bit = 0x80u; bit; bit >>= 1)
                *o++ = (uint8_t)(smk_pad_rgb[i][c] & bit ? 0xF0u : 0xC0u);
    }
    lcd_sync();                                    /* the LCD's DMA done, CS high: the SPI is free */
    fm1_padled_send(smk_padbuf, FM1_PADLED_FRAME);
    fm1_padled_done();                             /* ~0.5 ms; then the LCD's group and baud again */
}

/* the keys' velocity comes from the board (plat_key_vel): the FM-1's analog rows have no counterpart */
void plat_sense(int on) { (void)on; }
uint32_t plat_analog_mask(void) { return 0; }
