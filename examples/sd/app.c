/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Julius Vitkauskas */
/* The SD card example (app.h): one screen. At start and on PLAY: the card started (plat_sd_start), what it is, its
 * top folder. REC: the speed test, a 1 MiB file (FM1SD.BIN, made contiguous) written in 8 KiB pieces, read back and
 * checked, each way's speed shown; the file is left on the card. SEL: the free space counted (a big card whose FAT32
 * doesn't say it takes a while). SAVE: the card as a USB drive for a computer (plat_sd_disk; no files open here
 * meanwhile), SAVE again (or the computer's eject) back. The four buttons also come as USB MIDI notes (midi_buttons). Part of the unity build (device/unity.c, host/plat_host.c), after the
 * platform: gfx.c is in scope. */

#define SD_TEST_FILE "FM1SD.BIN"
#define SD_TEST_SIZE (1u << 20)
#define SD_TEST_PIECE 8192u

static struct {
    uint32_t btn;
    char msg[2][24];
    uint32_t msg_until;
    uint8_t redraw, disk;               /* disk: the card is a USB drive now */
    int start;                          /* plat_sd_start's answer */
    int nfiles;                         /* in the top folder (-1: not read) */
    char files[7][24];
    uint32_t file_kb[7];
    uint8_t file_dir[7];
    int test;                           /* 0 not run, 1 passed, -1 failed */
    uint32_t wr_kbs, rd_kbs;
    char test_why[24];
    int32_t free_mb;                    /* -1: not counted */
    uint8_t fat;                        /* FS_FAT12 .. FS_EXFAT, 0 none */
} ui;
static uint8_t piece[SD_TEST_PIECE] __attribute__((aligned(4))) PLAT_POOL;

void app_init(void) {}
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i;
    (void)master_q12;
    for (i = 0; i < 2u * n; i++)
        lr[i] = 0;
}
void app_panic(void) {}

static void copy(char *d, const char *s, uint32_t max)
{
    uint32_t i;
    for (i = 0; i + 1u < max && s && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

void app_say(const char *a, const char *b)
{
    copy(ui.msg[0], a, sizeof ui.msg[0]);
    copy(ui.msg[1], b, sizeof ui.msg[1]);
    ui.msg_until = plat_ms() + 1500u;
    ui.redraw = 1;
}

static char *put(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}
static char *put_int(char *d, int32_t v)
{
    char t[12];
    int k = 0;
    uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    if (v < 0)
        *d++ = '-';
    do {
        t[k++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    while (k)
        *d++ = t[--k];
    *d = 0;
    return d;
}

static void list(void)                  /* the top folder's first entries */
{
    DIR d;
    FILINFO fi;
    ui.nfiles = -1;
    if (ui.start != 0 || f_opendir(&d, "") != FR_OK)
        return;
    ui.nfiles = 0;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & (AM_HID | AM_SYS))
            continue;
        if (ui.nfiles < 7) {
            copy(ui.files[ui.nfiles], fi.fname, sizeof ui.files[0]);
            ui.file_kb[ui.nfiles] = (uint32_t)((fi.fsize + 1023u) / 1024u);
            ui.file_dir[ui.nfiles] = (uint8_t)(fi.fattrib & AM_DIR ? 1 : 0);
        }
        ui.nfiles++;
    }
    f_closedir(&d);
}

static void start(void)
{
    ui.start = plat_sd_start();
    ui.free_mb = -1;
    ui.fat = 0;
    list();
    if (ui.start == 0) {
        DIR d;                          /* the FAT type from the volume the folder is on */
        if (f_opendir(&d, "") == FR_OK) {
            ui.fat = d.obj.fs->fs_type;
            f_closedir(&d);
        }
    }
    app_say("SD CARD", plat_sd_what(ui.start ? ui.start : 1));
}

static uint8_t pat(uint32_t i) { return (uint8_t)(i * 7u + (i >> 8) * 13u + (i >> 16)); }

static int speed_test(void)             /* 0 passed; else ui.test_why says what failed */
{
    FIL f;
    UINT n;
    uint32_t i, at, t0;
    if (ui.start != 0) {
        copy(ui.test_why, "no files on the card", sizeof ui.test_why);
        return -1;
    }
    if (f_open(&f, SD_TEST_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK ||
        f_expand(&f, SD_TEST_SIZE, 1) != FR_OK) {   /* contiguous: as a sample to stream would be */
        f_close(&f);
        copy(ui.test_why, "no room (1 MiB)", sizeof ui.test_why);
        return -1;
    }
    t0 = plat_ms();
    for (at = 0; at < SD_TEST_SIZE; at += SD_TEST_PIECE) {
        for (i = 0; i < SD_TEST_PIECE; i++)
            piece[i] = pat(at + i);
        if (f_write(&f, piece, SD_TEST_PIECE, &n) != FR_OK || n != SD_TEST_PIECE) {
            f_close(&f);
            copy(ui.test_why, "write failed", sizeof ui.test_why);
            return -1;
        }
    }
    if (f_close(&f) != FR_OK) {
        copy(ui.test_why, "write failed", sizeof ui.test_why);
        return -1;
    }
    ui.wr_kbs = SD_TEST_SIZE / 1024u * 1000u / (plat_ms() - t0 + 1u);
    if (f_open(&f, SD_TEST_FILE, FA_READ) != FR_OK) {
        copy(ui.test_why, "file gone", sizeof ui.test_why);
        return -1;
    }
    t0 = plat_ms();
    for (at = 0; at < SD_TEST_SIZE; at += SD_TEST_PIECE) {
        if (f_read(&f, piece, SD_TEST_PIECE, &n) != FR_OK || n != SD_TEST_PIECE) {
            f_close(&f);
            copy(ui.test_why, "read failed", sizeof ui.test_why);
            return -1;
        }
        for (i = 0; i < SD_TEST_PIECE; i++)
            if (piece[i] != pat(at + i)) {
                f_close(&f);
                copy(ui.test_why, "read back wrong", sizeof ui.test_why);
                return -1;
            }
    }
    ui.rd_kbs = SD_TEST_SIZE / 1024u * 1000u / (plat_ms() - t0 + 1u);
    f_close(&f);
    return 0;
}

static void disk_mode(int on)
{
    if (on) {
        const plat_disk_t *d = plat_sd_disk();   /* (its files unmounted: the computer's now) */
        if (!d) {
            app_say("NO CARD", "PLAY looks again");
            return;
        }
        ui.disk = 1;
        plat_disk_mode(d);
        app_say("USB DRIVE", "SAVE: back");
    } else {
        plat_disk_mode(0);
        ui.disk = 0;
        start();                        /* the computer may have changed everything */
    }
}

void app_loop(void)
{
    if (ui.disk && plat_sd_disk_poll() == PLAT_DISK_EJECTED)
        disk_mode(0);
}

/* the buttons from a computer too, for tests at a distance: notes on USB MIDI, 60 PLAY, 62 REC, 64 SEL, 65 SAVE (as
 * amidi -p hw:N,0,0 -S '90 3C 7F') */
static uint32_t midi_buttons(void)
{
    static const uint8_t NOTE[4] = {60, 62, 64, 65}, BTN[4] = {B_PLAY, B_REC, B_SEL, B_SAVE};
    uint32_t pkt, down = 0;
    int port, i;
    while (plat_midi_in(&pkt, &port))
        if ((pkt >> 8 & 0xF0u) == 0x90u && (pkt >> 24 & 0x7Fu))
            for (i = 0; i < 4; i++)
                if ((pkt >> 16 & 0x7Fu) == NOTE[i])
                    down |= 1u << BTN[i];
    return down;
}

static void input(void)
{
    uint32_t btn = plat_buttons(), down = (btn & ~ui.btn) | midi_buttons();
    ui.btn = btn;
    if (down >> B_SAVE & 1u) {
        disk_mode(!ui.disk);
        return;
    }
    if (ui.disk)
        return;
    if (down >> B_PLAY & 1u)
        start();
    if (down >> B_REC & 1u) {
        app_say("SPEED TEST", "1 MiB");
        ui.test = speed_test() ? -1 : 1;
        list();
        app_say("SPEED TEST", ui.test > 0 ? "PASSED" : "FAILED");
    }
    if (down >> B_SEL & 1u && ui.start == 0) {
        DWORD fre;
        FATFS *fs;
        if (f_getfree("", &fre, &fs) == FR_OK)
            ui.free_mb = (int32_t)(fre * fs->csize / 2048u);
        ui.redraw = 1;
    }
}
void app_input_only(void) { input(); }

static void line(int32_t y, const char *t, uint16_t c, const char *right)
{
    cv_begin(240, 15, C_BLACK);
    cv_text(10, 1, &FONT_XS, t, c);
    if (right)
        cv_text(228 - text_w(&FONT_XS, right), 1, &FONT_XS, right, C_DIM);
    cv_blit(0, (uint32_t)y);
}

static void draw(void)
{
    static const char *const FAT[] = {"", "FAT12", "FAT16", "FAT32", "exFAT"};
    plat_sd_info_t in;
    char t[48], r[24], *q;
    int i, st = plat_sd_state();
    plat_sd_info(&in);
    cv_begin(240, 32, C_BLACK);
    if (plat_ms() < ui.msg_until) {
        int32_t x = cv_text(10, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        int32_t x = cv_text(10, 6, &FONT_B, APP_NAME, C_WHITE);
        cv_text(x + 8, 10, &FONT_XS, ui.disk ? (st == 1 ? "USB DRIVE" : "CARD LOST: PUT IT BACK") :
                plat_sd_what(st == 1 && ui.start ? ui.start : st), st == 1 ? C_HI : C_AMB);
    }
    cv_blit(0, 0);
    q = put(t, "Size ");
    if (in.sectors) {
        q = put_int(q, (int32_t)(in.sectors / 2048u));
        q = put(q, in.hc ? " MB  SDHC" : " MB  SDSC");
        if (ui.fat && ui.fat <= 4)
            put(put(q, "  "), FAT[ui.fat]);
    } else
        put(q, "-");
    line(34, t, in.sectors ? C_WHITE : C_GRAY, 0);
    q = put(t, "Clock ");
    q = put_int(q, (int32_t)(in.khz / 1000u));
    q = put(q, ".");
    q = put_int(q, (int32_t)(in.khz % 1000u / 100u));
    put(q, in.khz ? (in.dma ? " MHz  DMA" : " MHz  bytes") : " MHz");
    line(49, t, C_WHITE, 0);
    q = put(t, "CRC errors ");
    q = put_int(q, (int32_t)in.crc_errors);
    q = put(q, "  retries ");
    q = put_int(q, (int32_t)in.retries);
    q = put(q, "  timeouts ");
    put_int(q, (int32_t)in.timeouts);
    line(64, t, in.crc_errors || in.timeouts ? C_AMB : C_GRAY, 0);
    if (ui.test > 0) {
        q = put(t, "Test: write ");
        q = put_int(q, (int32_t)ui.wr_kbs);
        q = put(q, " KB/s  read ");
        q = put_int(q, (int32_t)ui.rd_kbs);
        put(q, " KB/s");
    } else
        put(put(t, "Test: "), ui.test < 0 ? ui.test_why : "REC runs it (1 MiB)");
    line(82, t, ui.test > 0 ? C_HI : ui.test < 0 ? C_AMB : C_GRAY, 0);
    r[0] = 0;
    if (ui.free_mb >= 0)
        put(put_int(r, ui.free_mb), " MB free");
    line(100, ui.nfiles < 0 ? "No files" : "Files (SEL counts the free space)", C_GRAY, r[0] ? r : 0);
    for (i = 0; i < 7; i++) {
        int have = i < ui.nfiles;
        r[0] = 0;
        if (have)
            put(ui.file_dir[i] ? r : put_int(r, (int32_t)ui.file_kb[i]), ui.file_dir[i] ? "folder" : " KB");
        line(116 + i * 15, have ? ui.files[i] : "", C_WHITE, have ? r : 0);
    }
    line(222, ui.disk ? "SAVE: back from the USB drive" : "PLAY look  REC test  SAVE USB drive", C_DIM, 0);
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    start();
    ui.redraw = 1;
}

void app_frame(void)
{
    static uint32_t last_draw;
    static int last_st;
    uint32_t now = plat_ms();
    input();
    if (plat_sd_state() != last_st) {   /* the card lost or found again (as a USB drive too) */
        last_st = plat_sd_state();
        if (last_st != 1) {             /* (what it showed of the card: gone with it) */
            ui.nfiles = -1;
            ui.fat = 0;
            ui.free_mb = -1;
        }
        ui.redraw = 1;
    }
    if ((ui.msg_until && now >= ui.msg_until) || (now - last_draw > 1000u && !ui.disk)) {   /* (as a USB drive: no
                                         * redraws, each would end the card's stream) */
        if (now >= ui.msg_until)
            ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        last_draw = now;
        draw();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "sd_state"))
        *got = ui.start;                /* plat_sd_start's last answer */
    else if (!strcmp(what, "sd_files"))
        *got = ui.nfiles;
    else if (!strcmp(what, "sd_test"))
        *got = ui.test;
    else
        return 0;
    return 1;
}
#endif
