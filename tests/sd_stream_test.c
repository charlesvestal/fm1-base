/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Henri David */
/* The SD card's streams for an app (plat.h plat_sd_read_open / _read_next / _write_open / _write_next / _close; device/sd.c)
 * against the modelled card (tests/sd_card_model.h): a run read whole and in pieces, a wait that never runs past its
 * budget while the card is slow to answer, the LCD drawing between blocks (a read opened again where it was, a write
 * kept open: the card deselected between two blocks, never left busy under the LCD), a damaged block, a card pulled or
 * stuck, the card's end, a plain call in the middle, a refused block.
 * With -DFM1B_SD_FINE=1 the start's finer clock steps (fm1.toml sd_clock_fine). With -DFM1B_SD_AHEAD=N the same with
 * the read ahead in SPI1's interrupt (device/sd_ahead.h; the interrupt run by the
 * test while a read waits, and between calls as a main loop held up by the render would let it run).
 *   cc -std=c99 -DPLAT_HOST -Iinclude -Ilib -Itests -o sd_stream_test tests/sd_stream_test.c && ./sd_stream_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "sd_card_model.h"

static uint32_t lcd_syncs, busy_lent;
static void sd_bus_lend(void);
static void lcd_sync(void)                 /* as device/lcd.c's: the bus lent to the panel first */
{
    sd_bus_lend();
    lcd_syncs++;
    if (sdm.open)
        sdm.lcd_overlap++;
    if (sdm.busy > 0 && !sdm.out_n)
        busy_lent++;                       /* (the card busy: its DO low against the panel's D/C between transfers) */
}
#define LCD_BAUD 0u
uint32_t plat_ms(void) { return sdm_ms; }
#if FM1B_SD_AHEAD
#define FM1_IRQ_SPI1 16u                   /* (felucca-bsp chip/fm1_irq.h's, stood in for) */
static uint32_t irq_masked = 1, irq_attached;
static void fm1_irq_attach(uint32_t n, void (*h)(void), uint32_t prio) { (void)n, (void)h, (void)prio; irq_attached++; }
static void fm1_irq_mask(uint32_t n) { (void)n; irq_masked = 1; }
static void fm1_irq_unmask(uint32_t n) { (void)n; irq_masked = 0; }
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
void isr_spi1(void) {}
static void irq_run(void);
#define SD_AHEAD_WAIT() irq_run()
#endif

#include "../device/sd.c"
#include "../lib/sd_fs.c"

#if FM1B_SD_AHEAD
static void irq_run(void)                  /* the interrupt, if one is pending; else time passes */
{
    if (sdm_irq_pending && !irq_masked) {
        fm1_spi1_irq();
        return;
    }
    if (++sdm.xfers % 64 == 0)
        sdm_ms++;
}
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void card(uint32_t sectors)
{
    free(sdm.img);
    memset(&sdm, 0, sizeof sdm);
    sdm.img = malloc((size_t)sectors * 512);
    for (uint32_t i = 0; i < sectors * 512u; i++)
        sdm.img[i] = (uint8_t)(i * 7u + (i >> 9) * 13u + (i >> 17));
    sdm.sectors = sectors;
    sdm.hc = 1;
    sdm.v2 = 1;
    sdm.idle = 1;
    sdm.init_left = 3;
    memset(&sd, 0, sizeof sd);
    memset(&sds, 0, sizeof sds);
    CHECK(plat_sd_start() == SD_E_NOFS);
}

static int same(const uint8_t *b, uint32_t sector) { return !memcmp(b, sdm.img + (size_t)sector * 512, 512); }

static int next(void *b)                   /* read_next until it isn't busy */
{
    int r, k = 0;
    while ((r = plat_sd_read_next(b, 200)) == SD_E_BUSY && k++ < 100000)
        ;
    return r;
}

static int put(const void *b)              /* write_next until it isn't busy */
{
    int r, k = 0;
    while ((r = plat_sd_write_next(b)) == SD_E_BUSY && k++ < 100000)
        ;
    return r;
}

static void reads(void)
{
    static uint8_t b[520] __attribute__((aligned(4)));
    plat_sd_info_t in;
    uint32_t i, c18, x0, busy, worst;
    int r;

    printf("sd stream: reads\n");
    card(131072);
    CHECK(plat_sd_read_next(b, 100) == SD_E_STATE);         /* nothing open */
    CHECK(plat_sd_read_open(131072, 0) == SD_E_RANGE);
    CHECK(plat_sd_read_open(131071, 2) == SD_E_RANGE);
    c18 = sdm.count[18];
    CHECK(plat_sd_read_open(1000, 8) == 0);
    for (i = 0; i < 8; i++)
        CHECK(next(b) == 0 && same(b, 1000 + i));
    CHECK(plat_sd_read_next(b, 100) == SD_E_RANGE);         /* past the run planned */
    CHECK(plat_sd_close() == 0 && plat_sd_close() == 0);
    CHECK(sdm.count[18] - c18 == 1 && sdm.phase != SDM_READ);

    CHECK(plat_sd_read_open(2000, 0) == 0);                 /* open-ended, into a buffer DMA can't take */
    for (i = 0; i < 20; i++)
        CHECK(next(b + 1) == 0 && same(b + 1, 2000 + i));
    CHECK(plat_sd_close() == 0);

    printf("sd stream: a card slow to answer: never waited past the budget\n");
    sdm.nac = 300;                                          /* ~4.7 ms of the model's clock before each block */
    CHECK(plat_sd_read_open(3000, 4) == 0);
    for (i = busy = worst = 0; i < 4; i++) {
        for (;;) {
            x0 = sdm.xfers;
            r = plat_sd_read_next(b, 500);
            if (r != SD_E_BUSY)
                break;
            busy++;
            if (sdm.xfers - x0 > worst)
                worst = sdm.xfers - x0;
        }
        CHECK(r == 0 && same(b, 3000 + i));
    }
    printf("  4 blocks: %u answers busy, the longest %u bytes (500 us = 32 here)\n", busy, worst);
#if FM1B_SD_AHEAD
    CHECK(busy >= 4 * 8);                                   /* (the blocks' DMA is the interrupt's, not the call's) */
#else
    CHECK(busy >= 4 * 8 && worst <= 33);
#endif
    CHECK(plat_sd_close() == 0);
    sdm.nac = 0;

    printf("sd stream: the LCD between blocks: opened again where it was\n");
    c18 = sdm.count[18];
    CHECK(plat_sd_read_open(4000, 12) == 0);
    for (i = 0; i < 12; i++) {
        CHECK(next(b) == 0 && same(b, 4000 + i));
        if (i % 4 == 3)
            lcd_sync();
    }
    CHECK(plat_sd_close() == 0);
    CHECK(sdm.count[18] - c18 == 3 && sdm.lcd_overlap == 0);

    printf("sd stream: a damaged block: SD_E_CRC, then read again; two in a row: a step down\n");
    plat_sd_info(&in);
    CHECK(in.khz == 20000);
    sdm.flip_next = 1;                                      /* (the model makes a block as it opens: set before) */
    CHECK(plat_sd_read_open(5000, 0) == 0);
    CHECK(next(b) == SD_E_CRC);
    CHECK(next(b) == 0 && same(b, 5000));
    CHECK(plat_sd_close() == 0);
    sdm.flip_next = 2;
    CHECK(plat_sd_read_open(5001, 0) == 0);
    CHECK(next(b) == SD_E_CRC);
    sdm.flip_next = 1;                                      /* (opened again: a fresh block, damaged too) */
    CHECK(next(b) == SD_E_CRC);
    CHECK(next(b) == 0 && same(b, 5001));
    plat_sd_info(&in);
    CHECK(in.khz == 15000 && in.crc_errors >= 3);
    CHECK(plat_sd_close() == 0);

    printf("sd stream: the card's end\n");
    CHECK(plat_sd_read_open(131070, 0) == 0);
    CHECK(next(b) == 0 && same(b, 131070));
    CHECK(next(b) == 0 && same(b, 131071));
    CHECK(plat_sd_read_next(b, 100) == SD_E_RANGE);
    CHECK(plat_sd_close() == 0 && sdm.phase != SDM_READ);

    printf("sd stream: a plain read in the middle\n");
    CHECK(plat_sd_read_open(6000, 0) == 0);
    CHECK(next(b) == 0 && same(b, 6000));
    CHECK(plat_sd_read(7, b, 1) == 0 && same(b, 7));
    CHECK(next(b) == 0 && same(b, 6001));
    CHECK(plat_sd_close() == 0);
}

#if FM1B_SD_AHEAD
static void ahead(void)
{
    static uint8_t b[512] __attribute__((aligned(4)));
    uint32_t i, k, x0, c12;
    printf("sd stream: read ahead: the card streams on while the main loop is held up\n");
    card(131072);
    CHECK(irq_attached == 1);
    CHECK(plat_sd_read_open(20000, 40) == 0);
    for (k = 0; k < 4000 && sdm_irq_pending; k++)         /* the main loop held up: only the interrupt runs */
        fm1_spi1_irq();
    CHECK(sdx.state == SDA_FULL && sdx.wr == FM1B_SD_AHEAD);
    for (i = 0; i < FM1B_SD_AHEAD; i++) {                   /* then the blocks come at once */
        x0 = sdm.xfers;
        CHECK(plat_sd_read_next(b, 0) == 0 && same(b, 20000 + i));
        CHECK(sdm.xfers - x0 <= 520u);                      /* (only the hunt a freed slot starts) */
    }
    for (; i < 40; i++)
        CHECK(next(b) == 0 && same(b, 20000 + i));
    CHECK(sdx.wr == 40 && sdx.state == SDA_FULL);           /* the run planned: not a block past it */
    c12 = sdm.count[12];
    CHECK(plat_sd_close() == 0 && sdm.count[12] - c12 == 1 && sdm.phase != SDM_READ && irq_masked);

    printf("sd stream: read ahead: no DMA, no read ahead\n");
    card(131072);
    sdm.dma_bad = 1;
    CHECK(plat_sd_start() == SD_E_NOFS);
    CHECK(plat_sd_read_open(100, 4) == 0 && !sd_ahead_on());
    for (i = 0; i < 4; i++)
        CHECK(next(b) == 0 && same(b, 100 + i));
    CHECK(plat_sd_close() == 0);
}
#endif

static void clock(void)                    /* a board whose ceiling falls between the coarse steps */
{
    plat_sd_info_t in;
    uint32_t t0;
    printf("sd stream: a card that reads right up to 9 MHz\n");
    card(131072);
    sdm.max_khz = 9000;
    t0 = sdm_ms;
    CHECK(plat_sd_start() == SD_E_NOFS);
    plat_sd_info(&in);
    printf("  %u kHz (sd_clock_fine %d), the start %u ms\n", in.khz, FM1B_SD_FINE, sdm_ms - t0);
    CHECK(in.khz == (FM1B_SD_FINE ? 8571u : 5000u) && sdm_ms - t0 < 4000u);
}

static void writes(void)
{
    static uint8_t w[512] __attribute__((aligned(4))), b[512] __attribute__((aligned(4)));
    uint32_t i, k, c25, c13, bw, busy, bad;
    int r;

    printf("sd stream: writes\n");
    card(131072);
    CHECK(plat_sd_write_next(w) == SD_E_STATE);
    CHECK(plat_sd_write_open(131072) == SD_E_RANGE);
    c25 = sdm.count[25];
    CHECK(plat_sd_write_open(9000) == 0);
    CHECK(plat_sd_write_next(0) == 0);                      /* asked: it takes one */
    for (i = busy = 0; i < 16; i++) {
        for (k = 0; k < 512; k++)
            w[k] = (uint8_t)(i * 3 + k);
        bw = sdm.blocks_written;
        while ((r = plat_sd_write_next(w)) == SD_E_BUSY) {
            busy++;
            CHECK(sdm.blocks_written == bw);                /* busy: nothing sent */
        }
        CHECK(r == 0);
    }
    CHECK(plat_sd_close() == 0);
    printf("  16 blocks, %u answers busy\n", busy);
    CHECK(busy > 0 && sdm.count[25] - c25 == 1 && sdm.phase == SDM_IDLE);
    for (i = bad = 0; i < 16; i++)
        for (k = 0; k < 512; k++)
            bad += sdm.img[(size_t)(9000 + i) * 512 + k] != (uint8_t)(i * 3 + k);
    CHECK(bad == 0);

    printf("sd stream: the LCD between blocks: the write kept open, the card never busy under it\n");
    c25 = sdm.count[25];
    c13 = sdm.count[13];
    CHECK(plat_sd_write_open(10000) == 0);
    for (i = 0; i < 30; i++) {
        for (k = 0; k < 512; k++)
            w[k] = (uint8_t)(i * 5 + k);
        CHECK(put(w) == 0);
        if (i % 3 == 2) {
            lcd_sync();
            lcd_sync();                                     /* (twice: parked already) */
        }
    }
    CHECK(plat_sd_close() == 0);
    CHECK(sdm.count[25] - c25 == 1 && sdm.count[13] - c13 == 1);   /* one stream, its status at the end */
    CHECK(sdm.lcd_overlap == 0 && busy_lent == 0);
    for (i = bad = 0; i < 30; i++)
        for (k = 0; k < 512; k++)
            bad += sdm.img[(size_t)(10000 + i) * 512 + k] != (uint8_t)(i * 5 + k);
    CHECK(bad == 0);

    printf("sd stream: a plain read while a write is parked: it ends it, the next block opens another\n");
    c25 = sdm.count[25];
    CHECK(plat_sd_write_open(11000) == 0);
    memset(w, 0x11, sizeof w);
    CHECK(put(w) == 0);
    lcd_sync();
    CHECK(plat_sd_read(7, b, 1) == 0 && !memcmp(b, sdm.img + 7 * 512, 512));
    memset(w, 0x22, sizeof w);
    CHECK(put(w) == 0);
    CHECK(plat_sd_close() == 0);
    CHECK(sdm.count[25] - c25 == 2 && sdm.img[11000u * 512] == 0x11 && sdm.img[11001u * 512] == 0x22);

    printf("sd stream: a block refused, a stream over another\n");
    CHECK(plat_sd_write_open(12000) == 0);
    CHECK(put(w) == 0);
    sdm.wflip_next = 1;
    r = put(w);
    CHECK(r == SD_E_CRC && sdm.phase != SDM_WRITE_ERR);     /* ended by CMD12 */
    CHECK(plat_sd_write_next(w) == SD_E_STATE);             /* the stream is over: the app knows which block */
    CHECK(plat_sd_close() == 0);
    CHECK(plat_sd_write_open(12100) == 0);
    CHECK(put(w) == 0);
    CHECK(plat_sd_read_open(13000, 1) == 0);                /* a read over an open write: the write ended first */
    CHECK(next(b) == 0 && same(b, 13000));
    CHECK(plat_sd_close() == 0 && sdm.img[12100u * 512] == 0x22 && sdm.phase == SDM_IDLE);
}

static void lost(void)
{
    static uint8_t b[512] __attribute__((aligned(4)));
    uint32_t t0;
    int r;

    printf("sd stream: a card pulled, a card stuck busy: errors in bounded time\n");
    card(131072);
    CHECK(plat_sd_read_open(1000, 0) == 0);
    CHECK(next(b) == 0);
    sdm.absent = 1;
    t0 = sdm_ms;
    while ((r = next(b)) == SD_E_CRC)                       /* (a block cut off: damaged; then nothing) */
        ;
    printf("  pulled while reading: %d after %u ms\n", r, sdm_ms - t0);
    CHECK((r == SD_E_TIMEOUT || r == SD_E_READ) && sdm_ms - t0 <= 300u);
    CHECK(plat_sd_read_next(b, 100) == SD_E_STATE);         /* the read is over */
    CHECK(plat_sd_close() == 0);
    sdm.absent = 0;
    sdm_power_cycle();
    CHECK(plat_sd_start() == SD_E_NOFS);

    CHECK(plat_sd_write_open(2000) == 0);
    CHECK(put(b) == 0);
    sdm.stuck = 1;
    t0 = sdm_ms;
    r = put(b);
    printf("  stuck while writing: %d after %u ms\n", r, sdm_ms - t0);
    CHECK(r == SD_E_TIMEOUT && sdm_ms - t0 <= 1300u);   /* (500 ms busy, then the stop's 500: the watchdog is 8 s) */
    CHECK(plat_sd_close() == 0);
    sdm.stuck = 0;
    sdm_power_cycle();
    CHECK(plat_sd_start() == SD_E_NOFS);
    CHECK(plat_sd_read(5, b, 1) == 0 && same(b, 5));
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
#if FM1B_SD_AHEAD
    sd_ahead_attach();                     /* (main_fm1.c's, at the start) */
    printf("sd stream: read ahead, %u slots\n", FM1B_SD_AHEAD);
#endif
    reads();
#if FM1B_SD_AHEAD
    ahead();
#endif
    clock();
    writes();
    lost();
    CHECK(sdm.crc7_bad == 0 && sdm.busy_talk == 0);
    if (fails) {
        printf("sd stream: %d FAILED\n", fails);
        return 1;
    }
    printf("sd stream: ok\n");
    return 0;
}
