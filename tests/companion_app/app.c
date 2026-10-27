/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Henri David */
/* The companion test app: see app.h */
static uint32_t cp_own, cp_commits;
static uint8_t playing;
static int32_t tempo = 120;

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
    if (n < 4u || p[0] != 0x7Du || p[1] != 0x43u || p[2] != 0x50u)
        return 0;                                    /* (not the app's: the platform's, the companion's) */
    cp_own++;
    if (p[3] == 4u && n >= 5u)
        tempo = 40 + p[4];                            /* (as the panel would: the computer is told, if it watches) */
    if (p[3] == 2u || p[3] == 3u)
        playing = p[3] == 2u;
    return 1;
}

/* the files: two in the data area (A / B copies), one the app makes */
static const plat_file_t NOTES = {0x97000u, 0x98000u, 0x1000u}, SAMPLE = {0x99000u, 0xA9000u, 16u * 0x1000u};
#define INFO_LEN 300u
static const plat_file_t *app_pf(uint32_t i) { return i == 0 ? &NOTES : i == 1 ? &SAMPLE : 0; }
int app_file(uint32_t i, plat_file_info_t *f)
{
    const plat_file_t *pf = app_pf(i);
    int n;
    if (i > 2u)
        return -1;
    if (i == 2u) {
        f->name = "info", f->kind = PLAT_FK_SETTINGS, f->flags = PLAT_FF_READ, f->size = f->room = INFO_LEN;
        return 0;
    }
    n = plat_file_size(pf);
    f->name = i ? "sample.raw" : "notes.txt";
    f->kind = i ? PLAT_FK_SAMPLE : PLAT_FK_TEXT;
    f->flags = PLAT_FF_READ | PLAT_FF_WRITE | PLAT_FF_DELETE | (i ? 0u : PLAT_FF_PLAYING);
    f->size = n < 0 ? 0u : (uint32_t)n;
    f->room = pf->n - PLAT_FILE_HEAD;
    return 0;
}
int app_file_read(uint32_t i, uint32_t off, uint8_t *dst, uint32_t n)
{
    uint32_t k;
    if (i == 2u) {                                   /* (the app's own: byte k is k * 7 + 3) */
        if (off > INFO_LEN || n > INFO_LEN - off)
            return -1;
        for (k = 0; k < n; k++)
            dst[k] = (uint8_t)((off + k) * 7u + 3u);
        return 0;
    }
    return app_pf(i) ? plat_file_read(app_pf(i), off, dst, n) : -1;
}
int app_file_begin(uint32_t i, uint32_t size) { return app_pf(i) ? plat_file_begin(app_pf(i), size) : -1; }
int app_file_write(uint32_t i, uint32_t off, const uint8_t *src, uint32_t n)
{
    return app_pf(i) ? plat_file_write(app_pf(i), off, src, n) : -1;
}
int app_file_commit(uint32_t i, uint32_t size, uint32_t crc)
{
    int rc = app_pf(i) ? plat_file_commit(app_pf(i), size, crc) : -1;
    cp_commits += rc == 0;
    return rc;
}
int app_file_delete(uint32_t i) { return app_pf(i) ? plat_file_delete(app_pf(i)) : -1; }
int app_file_busy(void) { return playing; }

/* the parameters: TEMPO's value is the app's own (cmd 4 changes it), the others kept here */
static const char *const WAVES[] = {"SAW", "SQR", "TRI", "SIN"};
static char mode_name[40][8];
static const char *mode_label[40];
static int32_t pv[6] = {120, 0, 0, 100, 37, 0};
static const plat_param_t PARAMS[6] = {
    {10, PLAT_PT_NUMBER, 0, 0, 40, 240, 120, "TEMPO", "BPM", 0},
    {11, PLAT_PT_NUMBER, 0, 0, 0, 100, 0, "SWING", "%", 0},
    {20, PLAT_PT_ENUM, 1, 0, 0, 3, 0, "WAVE", 0, WAVES},
    {21, PLAT_PT_NUMBER, 1, 0, 0, 127, 100, "CUTOFF", 0, 0},
    {30, PLAT_PT_NUMBER, 2, PLAT_PA_READONLY, 0, 100, 0, "LOAD", "%", 0},
    {40, PLAT_PT_ENUM, 1, 0, -20, 19, 0, "MODE", 0, mode_label},
};
int app_param(uint32_t i, plat_param_t *p)
{
    uint32_t k;
    if (i >= 6u)
        return -1;
    if (!mode_label[0])                              /* (40 labels: a page asks for them in parts) */
        for (k = 0; k < 40u; k++) {
            mode_name[k][0] = 'M', mode_name[k][1] = 'O', mode_name[k][2] = 'D', mode_name[k][3] = 'E';
            mode_name[k][4] = ' ', mode_name[k][5] = (char)('0' + k / 10u), mode_name[k][6] = (char)('0' + k % 10u);
            mode_label[k] = mode_name[k];
        }
    *p = PARAMS[i];
    return 0;
}
const char *app_param_group(uint32_t g)
{
    static const char *const G[] = {"GLOBAL", "SOUND", "INFO"};
    return g < 3u ? G[g] : 0;
}
int32_t app_param_get(uint32_t i) { return i == 0 ? tempo : i < 6u ? pv[i] : 0; }
int32_t app_param_set(uint32_t i, int32_t v)
{
    if (i == 0)
        tempo = v;
    else if (i == 3u)
        pv[i] = v & ~1;                              /* (CUTOFF: even values only, as an app may take a value its way) */
    else if (i < 6u)
        pv[i] = v;
    return app_param_get(i);
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "own"))
        *got = (int)cp_own;
    else if (!strcmp(what, "commits"))
        *got = (int)cp_commits;
    else
        return 0;
    return 1;
}
#endif
