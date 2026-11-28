/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal
 * Adapted from Felucca's panel.c, Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments. */
/* Physical panel: which matrix button / encoder carries which printed label (plat.h's
 * B_* / EN_*). HARDWARE CALIBRATION (hold OCT- and OCT+ while powering on) asks for each
 * label in turn; the table is kept in flash (OBJ_PANEL) and in .noinit. */
#ifndef FM1_NBTN
#define FM1_NBTN 14u                     /* the FM-1 board's button ids (the SMK's header defines its own) */
#endif
#if PLAT_SMK37
/* the SMK-37's printed labels for the FM-1's functions, then its own four buttons */
static const char *const B_NAME[NB] = {"FX", "SCALE", "CHORD", "REPEAT", "PARA", "GLOBE", "PATCH", "BT",
                                        "ARP", "SEQ", "SEQ PLAY", "SEQ REC", "OCTAVE-", "OCTAVE+",
                                        "KBANK", "FBANK", "<", ">"};
#if defined(APP_SMK_8ENC)
#define PANEL_ENC_RESERVED 0u
#else
#define PANEL_ENC_RESERVED (1u << 4)     /* K5: the virtual MASTER's knob */
#endif
#else
static const char *const B_NAME[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE",
                                        "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"};
#define PANEL_ENC_RESERVED 0u
#endif
#if PLAT_SMK37 && defined(APP_SMK_8ENC)
static const char *const E_NAME[NE] = {"KNOB 1", "KNOB 2", "KNOB 3", "KNOB 4", "KNOB 5",
                                        "KNOB 6", "KNOB 7", "KNOB 8"};
#else
static const char *const E_NAME[NE] = {"SELECT", "ALGORITHM", "PRESETS", "KNOB 1", "KNOB 2",
                                        "KNOB 3", "KNOB 4"};
#endif
#if PLAT_SMK37
#if defined(APP_SMK_8ENC)
#define PANEL_MAGIC 0x50414E37u          /* "PAN7": all eight encoders are calibrated */
#else
#define PANEL_MAGIC 0x50414E36u          /* "PAN6": the SMK's layout (18 buttons, 8 encoders, the master) */
#endif
#else
#define PANEL_MAGIC 0x50414E35u          /* "PAN5": Felucca's layout, so its calibration stays valid */
#endif

typedef struct {
    uint32_t magic;
    uint8_t btn[NB];             /* matrix button id (0..13) per label */
    uint8_t enc[NE];             /* matrix encoder (0..6) per role */
    int8_t dir[NE];              /* +1 / -1 so that clockwise is + */
#if PLAT_SMK37
    uint32_t master;             /* the virtual MASTER's level, 0..4096 (plat_smk37.c) */
#endif
} panel_t;
panel_t panel __attribute__((section(".noinit")));

#if PLAT_SMK37
static const panel_t PANEL_DEFAULT = {   /* the SMK's ids (felucca-bsp boards/smk37/fm1_input.h FM1_BTN, K1..K8) */
    PANEL_MAGIC,
    {14, 8, 9, 7, 13, 10, 12, 11, 6, 15, 16, 17, 4, 5, 0, 1, 2, 3},
#if defined(APP_SMK_8ENC)
    {0, 1, 2, 3, 4, 5, 6, 7},
    {1, 1, 1, 1, 1, 1, 1, 1},
#else
    {5, 6, 7, 0, 1, 2, 3},
    {1, 1, 1, 1, 1, 1, 1},
#endif
    2896u,                                       /* half gain: the level a firmware starts at on the FM-1 */
};
#else
static const panel_t PANEL_DEFAULT = {
    PANEL_MAGIC,
    {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1},
    {0, 1, 6, 2, 3, 4, 5},
    {1, 1, 1, 1, 1, 1, 1},
};
#endif

static int panel_valid(const panel_t *p)
{
    uint32_t i, ok = p->magic == PANEL_MAGIC;
    for (i = 0; ok && i < NB; i++)
        ok = p->btn[i] < FM1_NBTN;
    for (i = 0; ok && i < NE; i++)
        ok = p->enc[i] < FM1_NENC && !((PANEL_ENC_RESERVED >> p->enc[i]) & 1u) && (p->dir[i] == 1 || p->dir[i] == -1);
    return (int)ok;
}

static void panel_init(void)
{
#if PLAT_SMK37 && defined(APP_SMK_8ENC)
    /* PAN7 stores eight knob roles; migrate PAN6's seven roles plus MASTER so existing calibrations survive. */
    typedef struct {
        uint32_t magic;
        uint8_t btn[NB], enc[7];
        int8_t dir[7];
        uint32_t master;
    } panel_v6_t;
    union { panel_t current; panel_v6_t v6; } saved;
    int n = plat_store_load(OBJ_PANEL, &saved, sizeof saved);
    if (n == (int)sizeof saved.current && panel_valid(&saved.current)) {
        panel = saved.current;
    } else if (n == (int)sizeof saved.v6 && saved.v6.magic == 0x50414E36u) {
        panel_t p = PANEL_DEFAULT;
        uint32_t i;
        for (i = 0; i < NB; i++)
            p.btn[i] = saved.v6.btn[i];
        for (i = 0; i < 7; i++) {
            /* PAN6 stored SELECT/ALGORITHM/PRESETS before K1-K4; PAN7 orders K1-K8. */
            uint32_t role = i < 3 ? i + 5u : i - 3u;
            p.enc[role] = saved.v6.enc[i];
            p.dir[role] = saved.v6.dir[i];
        }
        p.master = saved.v6.master;
        if (panel_valid(&p))
            panel = p;
    }
#else
    panel_t p;
    if (plat_store_load(OBJ_PANEL, &p, sizeof p) == (int)sizeof p && panel_valid(&p))
        panel = p;
#endif
    if (!panel_valid(&panel))
        panel = PANEL_DEFAULT;
}

#define SETUP_IDLE_MS 30000u
static void panel_setup(void)
{
    uint32_t i, used = 0, t0 = fm1_ms;
    const panel_t old = panel;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    sys_box(0, 10, 240, SYS_S, "HARDWARE CALIBRATION", C_WHITE, 1);
    sys_box(0, 30, 240, SYS_S, "TEACH EACH BUTTON AND KNOB", C_GRAY, 1);
    while (fm1_in.buttons) {
        fm1_wdt_feed();
        if (fm1_ms - t0 > SETUP_IDLE_MS)
            goto timeout;
    }
    fm1_input_edges(0);
    for (i = 0; i < NB; i++) {
        uint32_t p = 0, id;
        sys_box(0, 80, 240, SYS_S, "PRESS", C_GRAY, 1);
        sys_box(0, 100, 240, SYS_BIG, B_NAME[i], C_WHITE, 1);
        t0 = fm1_ms;
        while (!(p & ~used)) {
            fm1_wdt_feed();
            p |= fm1_input_edges(0);
            if (fm1_ms - t0 > SETUP_IDLE_MS)
                goto timeout;
        }
        for (id = 0; id < FM1_NBTN; id++)
            if (((p & ~used) >> id) & 1u)
                break;
        panel.btn[i] = (uint8_t)id;
        used |= 1u << id;
    }
    used = PANEL_ENC_RESERVED;
    for (i = 0; i < NE; i++) {
        uint32_t e;
        int32_t st = 0;
        sys_box(0, 80, 240, SYS_S, "TURN RIGHT", C_GRAY, 1);
        sys_box(0, 100, 240, SYS_BIG, E_NAME[i], C_WHITE, 1);
        for (e = 0; e < FM1_NENC; e++)
            fm1_enc_take(e);
        t0 = fm1_ms;
        for (;;) {
            fm1_wdt_feed();
            if (fm1_ms - t0 > SETUP_IDLE_MS)
                goto timeout;
            for (e = 0; e < FM1_NENC; e++)
                if (!((used >> e) & 1u) && (st = fm1_enc_take(e)) != 0)
                    break;
            if (e < FM1_NENC)
                break;
        }
        panel.enc[i] = (uint8_t)e;
        panel.dir[i] = (int8_t)(st > 0 ? 1 : -1);
        used |= 1u << e;
        fm1_delay_ms(300);
        fm1_enc_take(e);
    }
    panel.magic = PANEL_MAGIC;
    plat_store_save(OBJ_PANEL, &panel, sizeof panel);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    return;
timeout:
    panel = old;
    lcd_fill(0, 0, 240, 240, C_BLACK);
}
