/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Julius Vitkauskas */
/* The SD card's layers on the host: device/sd.c (the protocol) against a modelled card (tests/sd_card_model.h, in
 * place of felucca-bsp's fm1_sd_hw.h), and lib/sd_fs.c (FatFs) on top. SDHC, SDSC v2 and v1 cards; sectors written
 * and read back one and many at a time, from aligned and unaligned buffers; a file system made, files written,
 * read back after a new start, long names, fast seek, a contiguous file; the clock's steps (a card that damages
 * blocks above 12 MHz ends at 10), DMA refused when its reads come damaged, a damaged block read again, no card.
 * Every command's CRC7 and every written block's CRC16 is checked by the model. With a path, the FAT32 card's image
 * is written there (run_tests.sh checks it with mtools when it has them).
 *   cc -std=c99 -DPLAT_HOST -DFF_USE_MKFS=1 -Iinclude -Ilib -Itests -o sd_test tests/sd_test.c && ./sd_test [IMAGE] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "sd_card_model.h"

static uint32_t lcd_syncs;
static void sd_bus_lend(void);
static void lcd_sync(void)                 /* as device/lcd.c's: a card stream ended first */
{
    sd_bus_lend();
    lcd_syncs++;
    if (sdm.open)
        sdm.lcd_overlap++;
}
#define LCD_BAUD 0u
uint32_t plat_ms(void) { return sdm_ms; }

#define FM1B_MSC 1                      /* plat_sd_disk, on a disk mode always mounted */
int plat_disk_poll(void) { return PLAT_DISK_MOUNTED; }
uint32_t plat_disk_idle_ms(void) { return 0; }

#include "../device/sd.c"
#include "../lib/sd_fs.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t crc7_bad, crc16_bad, cmds, wflips, busy_talk;   /* over every card */

static void card(uint32_t sectors, int hc, int v2)
{
    crc7_bad += sdm.crc7_bad;
    crc16_bad += sdm.crc16_bad;
    wflips += sdm.wflips;
    busy_talk += sdm.busy_talk;
    cmds += sdm.cmds;
    free(sdm.img);
    memset(&sdm, 0, sizeof sdm);
    sdm.img = calloc(sectors, 512);
    sdm.sectors = sectors;
    sdm.hc = hc;
    sdm.v2 = v2;
    sdm.idle = 1;
    sdm.init_left = 3;
}

static uint8_t pat(uint32_t sector, uint32_t i) { return (uint8_t)(sector * 7u + i * 13u + (i >> 9)); }

static void raw_test(const char *what)
{
    static uint8_t w[16 * 512 + 4] __attribute__((aligned(4))), r[16 * 512 + 4] __attribute__((aligned(4)));
    plat_sd_info_t in;
    uint32_t i, n;
    for (n = 1; n <= 16; n += 15)                 /* one sector, then a run of 16; aligned, then not */
        for (int off = 0; off < 2; off++) {
            uint32_t lba = 100 + n * 3 + (uint32_t)off * 40;
            for (i = 0; i < n * 512; i++)
                w[off + i] = pat(lba, i);
            CHECK(plat_sd_write(lba, w + off, n) == 0);
            memset(r, 0, sizeof r);
            CHECK(plat_sd_read(lba, r + 3 * off, n) == 0);
            CHECK(!memcmp(r + 3 * off, w + off, n * 512));
            CHECK(!memcmp(sdm.img + (size_t)lba * 512, w + off, n * 512));
        }
    CHECK(plat_sd_read(plat_sd_sectors() - 1, r, 2) == SD_E_RANGE);
    CHECK(plat_sd_read(plat_sd_sectors() - 1, r, 1) == 0);
    plat_sd_info(&in);
    printf("  %s: %u sectors, %u kHz, dma %u, hc %u, v%u, crc errors %u, retries %u\n", what, in.sectors, in.khz,
           in.dma, in.hc, in.ver, in.crc_errors, in.retries);
}

static void fs_test(int fmt, const char *img_out)
{
    static uint8_t work[4096], buf[70000], back[70000];
    MKFS_PARM opt = {(BYTE)fmt, 0, 0, 0, 0};
    FIL f;
    DIR d;
    FILINFO fi;
    UINT bw;
    uint32_t i, n = 0;
    DWORD clmt[64];
    CHECK(f_mkfs("", &opt, work, sizeof work) == FR_OK);
    CHECK(plat_sd_start() == 0);
    CHECK(f_mkdir("SAMPLES") == FR_OK);
    for (i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)(i * 31u + (i >> 8));
    static const struct { const char *name; UINT size; } FILES[] = {
        {"SAMPLES/kick.raw", 1}, {"SAMPLES/A rather long file name, kept.txt", 513}, {"SAMPLES/pad.qoa", 70000},
    };
    for (i = 0; i < 3; i++) {
        CHECK(f_open(&f, FILES[i].name, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
        CHECK(f_write(&f, buf, FILES[i].size, &bw) == FR_OK && bw == FILES[i].size);
        CHECK(f_close(&f) == FR_OK);
    }
    CHECK(f_open(&f, "BIG.BIN", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);   /* a contiguous file (streaming) */
    CHECK(f_expand(&f, 1u << 20, 1) == FR_OK);
    CHECK(f_close(&f) == FR_OK);
    plat_sd_stop();
    CHECK(plat_sd_state() == 0);
    CHECK(f_open(&f, FILES[0].name, FA_READ) != FR_OK);    /* unmounted */
    CHECK(plat_sd_start() == 0);                           /* again, as after a card change */
    CHECK(f_opendir(&d, "SAMPLES") == FR_OK);
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
        n++;
    f_closedir(&d);
    CHECK(n == 3);
    for (i = 0; i < 3; i++) {
        memset(back, 0, sizeof back);
        CHECK(f_open(&f, FILES[i].name, FA_READ) == FR_OK);
        CHECK(f_read(&f, back, sizeof back, &bw) == FR_OK && bw == FILES[i].size);
        CHECK(!memcmp(back, buf, FILES[i].size));
        if (FILES[i].size == 70000) {                       /* fast seek: the file's clusters in a table */
            f.cltbl = clmt;
            clmt[0] = sizeof clmt / sizeof clmt[0];
            CHECK(f_lseek(&f, CREATE_LINKMAP) == FR_OK);
            CHECK(f_lseek(&f, 40000) == FR_OK);
            CHECK(f_read(&f, back, 1000, &bw) == FR_OK && bw == 1000 && !memcmp(back, buf + 40000, 1000));
        }
        f_close(&f);
    }
    CHECK(f_stat("BIG.BIN", &fi) == FR_OK && fi.fsize == 1u << 20);
    if (img_out) {
        FILE *o = fopen(img_out, "wb");
        CHECK(o && fwrite(sdm.img, 512, sdm.sectors, o) == sdm.sectors);
        if (o)
            fclose(o);
    }
}

int main(int argc, char **argv)
{
    plat_sd_info_t in;
    static uint8_t r[512];
    setvbuf(stdout, 0, _IONBF, 0);                   /* (what was printed survives a crash) */

    printf("sd: SDHC 64 MiB\n");
    card(131072, 1, 1);
    CHECK(plat_sd_state() == 0);
    CHECK(plat_sd_read(0, r, 1) == SD_E_STATE);
    CHECK(plat_sd_start() == SD_E_NOFS);                   /* blank: sectors only */
    CHECK(plat_sd_state() == 1 && plat_sd_sectors() == 131072);
    plat_sd_info(&in);
    CHECK(in.khz == 20000 && in.dma == 1 && in.hc == 1 && in.ver == 2);
    raw_test("SDHC");
    fs_test(FM_FAT32, argc > 1 ? argv[1] : 0);

    printf("sd: SDSC v2 32 MiB\n");
    card(65536, 0, 1);
    CHECK(plat_sd_start() == SD_E_NOFS);
    plat_sd_info(&in);
    CHECK(in.hc == 0 && in.ver == 2 && in.sectors == 65536);
    raw_test("SDSC v2");
    fs_test(FM_FAT, 0);

    printf("sd: SDSC v1 16 MiB\n");
    card(32768, 0, 0);
    CHECK(plat_sd_start() == SD_E_NOFS);
    plat_sd_info(&in);
    CHECK(in.ver == 1 && in.sectors == 32768);
    raw_test("SDSC v1");
    fs_test(FM_FAT, 0);

    printf("sd: blocks damaged above 12 MHz\n");
    card(131072, 1, 1);
    sdm.max_khz = 12000;
    CHECK(plat_sd_start() == SD_E_NOFS);
    plat_sd_info(&in);
    CHECK(in.khz == 10000);
    raw_test("slow");

    printf("sd: DMA reads damaged\n");
    card(131072, 1, 1);
    sdm.dma_bad = 1;
    CHECK(plat_sd_start() == SD_E_NOFS);
    plat_sd_info(&in);
    CHECK(in.dma == 0 && in.khz == 20000);
    raw_test("no DMA");

    printf("sd: a block damaged once, then three times\n");
    card(131072, 1, 1);
    CHECK(plat_sd_start() == SD_E_NOFS);
    plat_sd_info(&in);
    {
        uint32_t c0 = in.crc_errors, r0 = in.retries;
        sdm.flip_next = 1;
        CHECK(plat_sd_read(5, r, 1) == 0 && !memcmp(r, sdm.img + 5 * 512, 512));
        plat_sd_info(&in);
        CHECK(in.crc_errors == c0 + 1 && in.retries == r0 + 1 && in.khz == 20000);
        sdm.flip_next = 3;
        CHECK(plat_sd_read(6, r, 1) == 0 && !memcmp(r, sdm.img + 6 * 512, 512));
        plat_sd_info(&in);
        CHECK(in.khz == 15000);                            /* two failures in a row: a step down */
        sdm.flip_next = 10;
        CHECK(plat_sd_read(7, r, 1) == SD_E_CRC);          /* never right: an error, not wrong data */
    }

    printf("sd: the card as a USB drive\n");
    card(131072, 1, 1);
    for (uint32_t i = 0; i < 131072u * 512u; i += 4)
        sdm.img[i] = (uint8_t)(i >> 9), sdm.img[i + 1] = (uint8_t)(i >> 17), sdm.img[i + 2] = (uint8_t)i;
    CHECK(plat_sd_start() == SD_E_NOFS);
    {
        const plat_disk_t *d = plat_sd_disk();
        static uint8_t blk[512];
        uint32_t i, k, c17, c18, bad = 0;
        CHECK(d && d->blocks == 131072 && d->sync);
        c17 = sdm.count[17], c18 = sdm.count[18];
        for (i = 1000; i < 3000; i++) {              /* sequential, as a computer reads a file */
            const uint8_t *b = d->read(i);
            if (!b || memcmp(b, sdm.img + (size_t)i * 512, 512))
                bad++;
            plat_sd_disk_poll();                     /* (read-ahead while USB would send) */
        }
        CHECK(bad == 0);
        printf("  2000 blocks read: %u CMD17, %u CMD18\n", sdm.count[17] - c17, sdm.count[18] - c18);
        CHECK(sdm.count[18] - c18 < 2000u / 4u && sdm.count[17] - c17 == 0);
        for (i = 131072 - 300, bad = 0; i < 131072; i++) {   /* up to the card's end: no read-ahead past it */
            const uint8_t *b = d->read(i);
            if (!b || memcmp(b, sdm.img + (size_t)i * 512, 512))
                bad++;
            plat_sd_disk_poll();
        }
        CHECK(bad == 0);
        c18 = sdm.count[25];
        for (i = 5000; i < 5100; i++) {              /* a WRITE of 100 blocks, then its end (sync) */
            for (k = 0; k < 512; k++)
                blk[k] = (uint8_t)(i * 3 + k);
            CHECK(d->write(i, blk) == 0);
        }
        CHECK(d->sync() == 0);
        CHECK(sdm.count[25] - c18 >= 1 && sdm.count[25] - c18 <= 2);   /* one stream (or two) */
        for (i = 5000, bad = 0; i < 5100; i++)
            for (k = 0; k < 512; k++)
                bad += sdm.img[(size_t)i * 512 + k] != (uint8_t)(i * 3 + k);
        CHECK(bad == 0);
        CHECK(d->read(6000) && d->read(6001));      /* read ahead over 6005, then 6005 written: read back new */
        for (k = 0; k < 512; k++)
            blk[k] = (uint8_t)(0xA5 ^ k);
        CHECK(d->write(6005, blk) == 0);
        CHECK(!memcmp(d->read(6005), blk, 512));     /* held: served from the hold */
        CHECK(d->sync() == 0);
        CHECK(!memcmp(d->read(6005), blk, 512) && !memcmp(sdm.img + 6005u * 512, blk, 512));
        CHECK(!memcmp(d->read(6006), sdm.img + 6006u * 512, 512));
    }
    CHECK(plat_sd_start() == SD_E_NOFS);            /* back: the card started again */

    printf("sd: the USB drive's streams, the LCD drawing in between\n");
    {
        const plat_disk_t *d = plat_sd_disk();
        static uint8_t blk[512];
        uint32_t i, k, c17, c18, c25, bad = 0;
        CHECK(d && d->work);
        c17 = sdm.count[17], c18 = sdm.count[18];
        for (i = 20000; i < 22000; i++) {            /* as disk mode: a block, then work while USB sends it */
            const uint8_t *b = d->read(i);
            if (!b || memcmp(b, sdm.img + (size_t)i * 512, 512))
                bad++;
            for (k = 0; k < 3; k++)
                d->work();
            if (i % 37 == 0)
                lcd_sync();                          /* the screen drawn: the stream ended */
            if (i % 500 == 0)
                plat_sd_disk_poll();
        }
        CHECK(bad == 0);
        printf("  2000 blocks read by stream: %u CMD18, %u CMD17\n", sdm.count[18] - c18, sdm.count[17] - c17);
        CHECK(sdm.count[17] - c17 == 0 && sdm.count[18] - c18 < 120);
        sdm.flip_next = 1;                           /* a block damaged in the stream: read again, plainly */
        for (i = 30000, bad = 0; i < 30100; i++) {
            const uint8_t *b = d->read(i);
            if (!b || memcmp(b, sdm.img + (size_t)i * 512, 512))
                bad++;
            d->work();
            d->work();
        }
        CHECK(bad == 0);
        c25 = sdm.count[25];
        for (i = 40000; i < 40300; i++) {            /* a WRITE of 300 blocks, the LCD drawing in between */
            for (k = 0; k < 512; k++)
                blk[k] = (uint8_t)(i * 5 + k);
            CHECK(d->write(i, blk) == 0);
            d->work();
            d->work();
            if (i % 41 == 0)
                lcd_sync();
        }
        CHECK(d->sync() == 0);
        printf("  300 blocks written by stream: %u CMD25\n", sdm.count[25] - c25);
        for (i = 40000, bad = 0; i < 40300; i++)
            for (k = 0; k < 512; k++)
                bad += sdm.img[(size_t)i * 512 + k] != (uint8_t)(i * 5 + k);
        CHECK(bad == 0);
        for (k = 0; k < 512; k++)                    /* written, then read straight after (the queue first) */
            blk[k] = (uint8_t)(0x3C ^ k);
        CHECK(d->write(41000, blk) == 0);
        CHECK(d->read(41000) && !memcmp(d->read(41000), blk, 512));
        CHECK(d->sync() == 0);
        CHECK(!memcmp(sdm.img + 41000u * 512, blk, 512));
        CHECK(d->read(50000) != 0);                  /* a read stream open, then a plain read: it ends it first */
        d->work();
        CHECK(plat_sd_read(7, r, 1) == 0 && !memcmp(r, sdm.img + 7 * 512, 512));
        CHECK(d->read(50001) && !memcmp(d->read(50001), sdm.img + 50001u * 512, 512));
    }
    CHECK(plat_sd_start() == SD_E_NOFS);

    printf("sd: a card pulled, a card stuck busy: errors in bounded time (the watchdog is 8 s)\n");
    {
        const plat_disk_t *d;
        uint32_t i, k, t0, nfail = 0, worst = 0;
        static uint8_t blk[512];
        card(131072, 1, 1);
        CHECK(plat_sd_start() == SD_E_NOFS);
        d = plat_sd_disk();
        for (i = 1000; i < 1100; i++) {
            CHECK(d->read(i) != 0);
            d->work();
        }
        sdm.absent = 1;                              /* pulled in the middle of a READ of 240 blocks */
        t0 = sdm_ms;
        for (i = 1100; i < 1340; i++) {
            uint32_t t1 = sdm_ms;
            nfail += d->read(i) == 0;
            d->work();
            if (sdm_ms - t1 > worst)
                worst = sdm_ms - t1;
        }
        printf("  pulled: %u of 240 reads failed, %u ms in all, %u ms the worst\n", nfail, sdm_ms - t0, worst);
        CHECK(nfail > 200 && sdm_ms - t0 < 3000u && plat_sd_state() < 0);
        sdm.absent = 0;                              /* put back: found again */
        sdm_power_cycle();
        CHECK(plat_sd_start() == SD_E_NOFS && plat_sd_state() == 1);

        d = plat_sd_disk();
        t0 = sdm_ms;
        for (i = 60000; i < 60100; i++) {            /* stuck busy in the middle of a WRITE of 100 blocks */
            for (k = 0; k < 512; k++)
                blk[k] = (uint8_t)k;
            if (i == 60030)
                sdm.stuck = 1;
            d->write(i, blk);
            d->work();
        }
        CHECK(d->sync() != 0);                       /* the computer hears it failed */
        printf("  stuck while writing: %u ms to the error\n", sdm_ms - t0);
        CHECK(sdm_ms - t0 < 5000u && plat_sd_state() < 0);
        t0 = sdm_ms;
        CHECK(plat_sd_read(5, blk, 1) == SD_E_STATE && sdm_ms - t0 < 2u);   /* lost: at once */
        sdm.stuck = 0;
        sdm_power_cycle();
        CHECK(plat_sd_start() == SD_E_NOFS);

        sdm.stuck = 1;                               /* one plain read: its budget */
        t0 = sdm_ms;
        CHECK(plat_sd_read(5, blk, 1) != 0);
        printf("  stuck, one plain read: %u ms\n", sdm_ms - t0);
        CHECK(sdm_ms - t0 <= 1700u && plat_sd_state() < 0);
        t0 = sdm_ms;                                 /* a start with the card stuck: its budget */
        CHECK(plat_sd_start() != 0);
        printf("  stuck, a start: %u ms\n", sdm_ms - t0);
        CHECK(sdm_ms - t0 <= 4700u);
        sdm.stuck = 0;
        sdm_power_cycle();
        CHECK(plat_sd_start() == SD_E_NOFS);         /* unstuck: back */

        d = plat_sd_disk();                          /* as a USB drive, the computer quiet: pulled, noticed */
        CHECK(d->present && d->present());
        CHECK(d->read(70000) != 0);
        sdm.absent = 1;
        t0 = sdm_ms;
        while (d->present() && sdm_ms - t0 < 3000u) {
            plat_sd_disk_poll();
            fm1_sd_xfer(0xFF);
        }
        printf("  pulled while the computer was quiet: noticed after %u ms\n", sdm_ms - t0);
        CHECK(!d->present() && sdm_ms - t0 <= 1200u);
        CHECK(d->read(70001) == 0);
        t0 = sdm_ms;
        while (sdm_ms - t0 < 2500u) {                /* the poll looks for it (no card: quick each time) */
            plat_sd_disk_poll();
            fm1_sd_xfer(0xFF);                       /* (the model's clock runs on bytes) */
        }
        CHECK(!d->present());
        sdm.absent = 0;
        sdm_power_cycle();
        t0 = sdm_ms;
        while (!d->present() && sdm_ms - t0 < 5000u) {
            plat_sd_disk_poll();
            fm1_sd_xfer(0xFF);
        }
        CHECK(d->present() && d->blocks == 131072);
        CHECK(d->read(70000) && !memcmp(d->read(70000), sdm.img + 70000u * 512, 512));
    }

    printf("sd: a written block damaged, a stop refused\n");
    {
        static uint8_t w[8 * 512] __attribute__((aligned(4))), rb[8 * 512] __attribute__((aligned(4)));
        const plat_disk_t *d;
        uint32_t i, k, c12, bad = 0;
        card(131072, 1, 1);
        CHECK(plat_sd_start() == SD_E_NOFS);
        for (i = 0; i < sizeof w; i++)
            w[i] = (uint8_t)(i * 11u + 3u);
        sdm.wflip_next = 1;                          /* a multi-block write: the 1st block refused, CMD12, again */
        c12 = sdm.count[12];
        CHECK(plat_sd_write(900, w, 8) == 0 && !memcmp(sdm.img + 900u * 512, w, sizeof w));
        CHECK(sdm.count[12] > c12 && sdm.phase != SDM_WRITE_ERR);
        sdm.rej12 = 1;                               /* a multi-block read whose stop is refused: again, right */
        CHECK(plat_sd_read(900, rb, 8) == 0 && !memcmp(rb, w, sizeof w));
        CHECK(sdm.phase != SDM_READ);                /* (not left streaming) */
        CHECK(plat_sd_read(5, rb, 1) == 0 && !memcmp(rb, sdm.img + 5 * 512, 512));   /* (in step after) */
        d = plat_sd_disk();                          /* the USB drive: a block refused mid-stream */
        sdm.wflip_next = 1;
        for (i = 80000; i < 80040; i++) {
            for (k = 0; k < 512; k++)
                w[k] = (uint8_t)(i + k);
            CHECK(d->write(i, w) == 0);
            d->work();
        }
        CHECK(d->sync() == 0);
        for (i = 80000; i < 80040; i++)
            for (k = 0; k < 512; k++)
                bad += sdm.img[(size_t)i * 512 + k] != (uint8_t)(i + k);
        CHECK(bad == 0 && sdm.phase != SDM_WRITE_ERR);
        CHECK(d->read(81000) != 0);                  /* a read stream, then its stop refused at the close */
        d->work();
        sdm.rej12 = 1;
        CHECK(plat_sd_read(6, rb, 1) == 0 && !memcmp(rb, sdm.img + 6 * 512, 512));
        CHECK(sdm.phase != SDM_READ);
        CHECK(plat_sd_start() == SD_E_NOFS);
    }

    printf("sd: a restart with the card left streaming (a read, a write between blocks, in a block, refused)\n");
    {
        static uint8_t rb[512] __attribute__((aligned(4))), w[512], was[8 * 512];
        const plat_disk_t *d;
        uint32_t t0;
        int half;
        card(131072, 1, 1);
        CHECK(plat_sd_start() == SD_E_NOFS);
        d = plat_sd_disk();                          /* the USB drive's read stream open (CMD18) ... */
        CHECK(d->read(5000) != 0);
        d->work();
        CHECK(sdm.phase == SDM_READ);
        memset(&sd, 0, sizeof sd);                   /* ... and the chip restarted (an install, a watchdog): the
                                                      * card not, still streaming */
        memset(&sds, 0, sizeof sds);
        CHECK(plat_sd_start() == SD_E_NOFS && plat_sd_state() == 1);
        CHECK(plat_sd_read(7, rb, 1) == 0 && !memcmp(rb, sdm.img + 7 * 512, 512));
        for (half = 0; half < 2; half++) {           /* a write stream open (CMD25): between blocks, in one */
            uint32_t k;
            CHECK(plat_sd_start() == SD_E_NOFS);
            d = plat_sd_disk();
            memset(w, 0x5A, sizeof w);
            for (k = 0; k < 4; k++) {
                d->write(90000 + k, w);
                d->work();
            }
            CHECK(sdm.phase == SDM_WRITE_WAIT);
            memcpy(was, sdm.img + 90000u * 512, sizeof was);
            if (half) {                              /* (the chip restarted 200 bytes into a block) */
                fm1_sd_xfer(0xFCu);
                for (k = 0; k < 200; k++)
                    fm1_sd_xfer(0xA5u);
                sdm.wflips++;                        /* (its CRC16 refused on purpose) */
            }
            memset(&sd, 0, sizeof sd);
            memset(&sds, 0, sizeof sds);
            CHECK(plat_sd_start() == SD_E_NOFS && plat_sd_state() == 1);
            CHECK(plat_sd_read(7, rb, 1) == 0 && !memcmp(rb, sdm.img + 7 * 512, 512));
            CHECK(!memcmp(was, sdm.img + 90000u * 512, sizeof was));   /* the half block never written */
        }
        {                                            /* a write block refused, then the restart: CMD12's busy (400 ms
                                                      * here, 500 allowed) waited for, no CMD0 to a busy card */
            uint32_t c0, k;
            CHECK(plat_sd_start() == SD_E_NOFS);
            sd_begin();
            CHECK(sd_cmd(25u, 90010u) == 0);
            fm1_sd_xfer(0xFCu);
            for (k = 0; k < 514; k++)
                fm1_sd_xfer(k < 512 ? 0x00u : 0xFFu);   /* (zeros, their CRC16 0000 sent as FFFF: refused) */
            sdm.wflips++;
            CHECK(sdm.phase == SDM_WRITE_ERR);
            memset(&sd, 0, sizeof sd);
            memset(&sds, 0, sizeof sds);
            c0 = sdm.count[0];
            t0 = sdm_ms;
            CHECK(plat_sd_start() == SD_E_NOFS && plat_sd_state() == 1);
            CHECK(sdm_ms - t0 >= 400u && sdm.count[0] > c0 && sdm.busy_talk == 0);
        }
        {                                            /* the limit: a half block whose 0xFF tail makes its CRC right
                                                      * (1 in 65536) is written, as a write cut by a power loss; the
                                                      * card starts all the same */
            static const uint8_t tail[2] = {0x61u, 0x4Fu};   /* (198 x A5, 61 4F, then 312 x FF: CRC16 FFFF) */
            uint32_t k, at;
            CHECK(plat_sd_start() == SD_E_NOFS);
            d = plat_sd_disk();
            for (k = 0; k < 4; k++) {
                d->write(90017 + k, w);
                d->work();
            }
            CHECK(sdm.phase == SDM_WRITE_WAIT);
            at = sdm.addr;                           /* (the sector the half block goes to) */
            fm1_sd_xfer(0xFCu);
            for (k = 0; k < 200; k++)
                fm1_sd_xfer(k < 198 ? 0xA5u : tail[k - 198]);
            memset(&sd, 0, sizeof sd);
            memset(&sds, 0, sizeof sds);
            CHECK(plat_sd_start() == SD_E_NOFS && plat_sd_state() == 1);
            CHECK(sdm.img[(size_t)at * 512] == 0xA5u && sdm.img[(size_t)at * 512 + 511] == 0xFFu);
        }
    }

    printf("sd: no card\n");
    card(131072, 1, 1);
    sdm.absent = 1;
    CHECK(plat_sd_start() == SD_E_NOCARD);
    CHECK(plat_sd_state() == SD_E_NOCARD);
    CHECK(plat_sd_read(0, r, 1) == SD_E_STATE);
    CHECK(!strcmp(plat_sd_what(plat_sd_state()), "no card"));

    card(1024, 1, 1);                                      /* (the last card's counts in) */
    CHECK(crc7_bad == 0 && crc16_bad == wflips);       /* (only the blocks damaged on purpose refused) */
    CHECK(sdm.lcd_overlap == 0 && lcd_syncs > 0);
    CHECK(sdm_wdt_feeds > 1000);                           /* (every plain read and write fed the watchdog) */
    CHECK(busy_talk == 0);                                 /* (nothing said to a busy card: it isn't heard) */
    printf("sd: %u commands; every command's CRC7 and written block's CRC16 accepted\n", cmds);
    if (fails) {
        printf("sd: %d FAILED\n", fails);
        return 1;
    }
    printf("sd: ok\n");
    return 0;
}
