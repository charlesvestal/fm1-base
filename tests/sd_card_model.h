/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Julius Vitkauskas */
/* A microSD card in SPI mode, byte by byte, in place of felucca-bsp's fm1_sd_hw.h (tests/sd_test.c: device/sd.c
 * against it). It answers as the SD Physical Layer spec's SPI mode does: CMD0 (its CRC always checked), CMD8, CMD55 /
 * ACMD41 (busy for a few tries), CMD58, CMD16, CMD59 (then every command's CRC7 and every written block's CRC16
 * checked), CMD9 (CSD 1.0 or 2.0), CMD17 / CMD18 / CMD12, CMD24 / CMD25 (data response, busy), CMD13; others are
 * illegal. Faults to set: a clock above max_khz damages a bit of each block read, DMA reads (fm1_sd_rx) arrive
 * damaged with dma_bad, the next n blocks read come damaged (flip_next), no card (absent), each block read late by
 * nac bytes (the card's access time before its start token). */
#pragma once
#include <stdint.h>
#include <string.h>

#define SDM_LSB_KHZ 60000u
enum { SDM_IDLE, SDM_READ, SDM_WRITE_WAIT, SDM_WRITE_DATA, SDM_WRITE_ERR };   /* WRITE_ERR: a block refused in a
                                         * multi-block write, the card waits for CMD12 (and nothing else) */
static struct {
    uint8_t *img;                       /* sectors x 512 */
    uint32_t sectors;
    int hc, v2, absent, dma_bad, flip_next, stuck;   /* stuck: DO held low, nothing heard (a wedged card) */
    int wflip_next, rej12;              /* the next n written blocks arrive damaged; the next n CMD12s refused (the
                                         * stream goes on) */
    uint32_t wflips;                    /* written blocks damaged so far (their CRC16 refused on purpose) */
    uint32_t busy_talk;                 /* bytes other than 0xFF sent while busy (unheard: a command lost) */
    uint32_t max_khz;                   /* 0: any clock */
    int nac;                            /* 0xFF bytes before each block read (its access time; up to 3000) */
    /* state */
    int idle, crc_on, acmd, init_left, open, sel;
    uint32_t baud;
    uint8_t cmd[6];
    int cmd_n;
    uint8_t out[4096];
    int out_h, out_n;
    int busy;                           /* bytes of 0x00 (busy) still to give */
    int phase, multi;                   /* SDM_*; a multi-block transfer */
    uint32_t addr;                      /* the next sector of a read or write */
    uint8_t wbuf[514];
    int wn;
    /* counts, for the test */
    uint32_t cmds, crc7_bad, crc16_bad, blocks_read, blocks_written, xfers, opens, lcd_overlap;
    uint32_t count[64];                 /* commands by number (ACMD23 at 23) */
} sdm;
static uint32_t sdm_ms;                 /* the test's plat_ms: one ms every 64 bytes on the wire */
#define SD_TICKS() (sdm.xfers * 1000u / 64u)   /* (device/sd.c's fine clock: here us, on the same bytes) */
#define SD_TICKS_PER_US 1u
static uint32_t fm1_sd_timeouts;
static uint32_t sdm_wdt_feeds;
static void fm1_wdt_feed(void) { sdm_wdt_feeds++; }   /* (felucca-bsp fm1_sys.h's, counted) */

static uint8_t sdm_crc7(const uint8_t *p, int n)
{
    uint32_t c = 0, b;
    for (int i = 0; i < n; i++)
        for (b = 0x80; b; b >>= 1) {
            c <<= 1;
            if ((c & 0x80) ^ (p[i] & b ? 0x80u : 0))
                c ^= 0x09;
        }
    return (uint8_t)(c & 0x7F);
}

static uint16_t sdm_crc16(const uint8_t *p, int n)   /* bit by bit: independent of device/sd.c's table */
{
    uint32_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= (uint32_t)p[i] << 8;
        for (int b = 0; b < 8; b++)
            c = c & 0x8000 ? (c << 1 ^ 0x1021) & 0xFFFF : c << 1 & 0xFFFF;
    }
    return (uint16_t)c;
}

static void sdm_put(uint8_t b)
{
    if (sdm.out_n < (int)sizeof sdm.out)
        sdm.out[(sdm.out_h + sdm.out_n++) % (int)sizeof sdm.out] = b;
}

static uint32_t sdm_khz(void) { return SDM_LSB_KHZ / (sdm.baud + 1); }

static void sdm_block(const uint8_t *d, int n)  /* a data block out: token, data (a bit damaged if asked), CRC16 */
{
    uint16_t c = sdm_crc16(d, n);
    int flip = (sdm.max_khz && sdm_khz() > sdm.max_khz) || (sdm.flip_next > 0 && n == 512);
    if (sdm.flip_next > 0 && n == 512)
        sdm.flip_next--;
    sdm_put(0xFF);
    for (int i = 0; n == 512 && i < sdm.nac; i++)
        sdm_put(0xFF);
    sdm_put(0xFE);
    for (int i = 0; i < n; i++)
        sdm_put((uint8_t)(d[i] ^ (flip && i == n / 3 ? 0x10 : 0)));
    sdm_put((uint8_t)(c >> 8));
    sdm_put((uint8_t)c);
}

static void sdm_read_next(void)
{
    if (sdm.addr >= sdm.sectors) {
        sdm_put(0x08);                  /* an error token: out of range */
        sdm.phase = SDM_IDLE;
        return;
    }
    sdm_block(sdm.img + (size_t)sdm.addr * 512, 512);
    sdm.addr++;
    sdm.blocks_read++;
}

static void sdm_r1(uint8_t r) { sdm_put(0xFF); sdm_put(r); }

static void sdm_command(void)
{
    uint8_t c = sdm.cmd[0] & 0x3F;
    uint32_t arg = (uint32_t)sdm.cmd[1] << 24 | (uint32_t)sdm.cmd[2] << 16 | (uint32_t)sdm.cmd[3] << 8 | sdm.cmd[4];
    int acmd = sdm.acmd;
    uint8_t idle = (uint8_t)sdm.idle;
    sdm.cmds++;
    sdm.count[c]++;
    sdm.acmd = 0;
    if ((c == 0 || c == 8 || sdm.crc_on) && (sdm.cmd[5] >> 1) != sdm_crc7(sdm.cmd, 5)) {
        sdm.crc7_bad++;
        sdm_r1((uint8_t)(idle | 0x08));
        return;
    }
    if (c == 12 && sdm.rej12 > 0) {     /* refused: its answer at once, then the read goes on (the next block) */
        sdm.rej12--;
        sdm.out_n = 0;
        sdm_put(0xFF);
        sdm_put(0x04);
        return;
    }
    if (sdm.phase == SDM_READ && c != 12)
        return;                         /* (a multi-block read heeds CMD12 only: a CMD0 gets its data, as on an
                                         * FM-1's card left streaming by the firmware before a restart) */
    if (sdm.phase == SDM_WRITE_ERR && c != 12) {   /* (only CMD12 ends a refused multi-block write) */
        sdm_r1(0x04);
        return;
    }
    if (c == 12) {                      /* stop: whatever was going out, dropped */
        int refused = sdm.phase == SDM_WRITE_ERR;
        sdm.out_n = 0;
        sdm.phase = SDM_IDLE;
        sdm_put(0xFF);                  /* the stuff byte */
        sdm_put(0x00);
        sdm.busy = refused ? 400 * 64 : 3;   /* (after a refused write block: busy 400 ms, as an SDXC card may be,
                                              * up to 500) */
        return;
    }
    if (c != 0 && sdm.idle && !(c == 8 || c == 55 || c == 58 || c == 59 || (acmd && c == 41))) {
        sdm_r1(idle | 0x04);
        return;
    }
    switch (c) {
    case 0:
        sdm.idle = 1;
        sdm.crc_on = 0;
        sdm.phase = SDM_IDLE;
        sdm.out_n = 0;
        sdm.busy = 0;
        sdm_r1(0x01);
        return;
    case 8:
        if (!sdm.v2) {
            sdm_r1(0x05);
            return;
        }
        sdm_r1(idle);
        sdm_put(0x00); sdm_put(0x00); sdm_put((uint8_t)(arg >> 8 & 0x0F)); sdm_put((uint8_t)arg);
        return;
    case 55:
        sdm.acmd = 1;
        sdm_r1(idle);
        return;
    case 23:                            /* ACMD23: blocks to pre-erase before a CMD25 */
        if (!acmd)
            break;
        sdm_r1(0);
        return;
    case 41:
        if (!acmd)
            break;
        if (sdm.hc && !(arg & 1u << 30)) {   /* an SDHC card stays busy for a host that doesn't say HCS */
            sdm_r1(0x01);
            return;
        }
        if (sdm.init_left > 0)
            sdm.init_left--;
        else
            sdm.idle = 0;
        sdm_r1((uint8_t)sdm.idle);
        return;
    case 58:
        sdm_r1(idle);
        sdm_put((uint8_t)((sdm.idle ? 0 : 0x80) | (sdm.hc && !sdm.idle ? 0x40 : 0)));
        sdm_put(0xFF); sdm_put(0x80); sdm_put(0x00);
        return;
    case 59:
        sdm.crc_on = arg & 1;
        sdm_r1(idle);
        return;
    case 16:
        sdm_r1(arg == 512 ? 0 : 0x40);
        return;
    case 9: {
        uint8_t csd[16] = {0};
        if (sdm.hc) {                   /* CSD 2.0: C_SIZE = sectors / 1024 - 1 */
            uint32_t cs = sdm.sectors / 1024 - 1;
            csd[0] = 0x40;
            csd[7] = (uint8_t)(cs >> 16 & 0x3F); csd[8] = (uint8_t)(cs >> 8); csd[9] = (uint8_t)cs;
        } else {                        /* CSD 1.0: READ_BL_LEN 9, C_SIZE_MULT 7: (C_SIZE + 1) x 512 sectors */
            uint32_t cs = sdm.sectors / 512 - 1;
            csd[5] = 0x09;
            csd[6] = (uint8_t)(cs >> 10 & 3); csd[7] = (uint8_t)(cs >> 2); csd[8] = (uint8_t)(cs << 6);
            csd[9] = 0x03; csd[10] = 0x80;
        }
        sdm_r1(0);
        sdm_block(csd, 16);
        return;
    }
    case 13:
        sdm_r1(0);
        sdm_put(0x00);
        return;
    case 17: case 18: case 24: case 25: {
        uint32_t s = sdm.hc ? arg : arg / 512;
        if (!sdm.hc && arg % 512) {
            sdm_r1(0x20);               /* address error */
            return;
        }
        if (s >= sdm.sectors) {
            sdm_r1(0x40);               /* parameter error */
            return;
        }
        sdm.addr = s;
        sdm.multi = c == 18 || c == 25;
        sdm_r1(0);
        if (c == 17 || c == 18) {
            sdm.phase = SDM_READ;
            sdm_read_next();
            if (c == 17)
                sdm.phase = SDM_IDLE;
        } else {
            sdm.phase = SDM_WRITE_WAIT;
            sdm.wn = 0;
        }
        return;
    }
    }
    sdm_r1(idle | 0x04);                /* illegal */
}

static void sdm_in(uint8_t b)           /* a byte the host sent */
{
    if (sdm.phase == SDM_WRITE_WAIT) {
        if (b == 0xFE || b == 0xFC) {
            sdm.phase = SDM_WRITE_DATA;
            sdm.wn = 0;
        } else if (b == 0xFD && sdm.multi) {
            sdm.phase = SDM_IDLE;       /* stop token: busy a while */
            sdm_put(0xFF);
            sdm.busy = 4;
        }
        return;
    }
    if (sdm.phase == SDM_WRITE_DATA) {
        sdm.wbuf[sdm.wn++] = b;
        if (sdm.wn < 514)
            return;
        if (sdm.wflip_next > 0) {       /* damaged on the way */
            sdm.wflip_next--;
            sdm.wflips++;
            sdm.wbuf[100] ^= 0x01;
        }
        if (sdm.crc_on && (uint16_t)(sdm.wbuf[512] << 8 | sdm.wbuf[513]) != sdm_crc16(sdm.wbuf, 512)) {
            sdm.crc16_bad++;
            sdm_put(0x0B);              /* data response: CRC error */
            sdm.phase = sdm.multi ? SDM_WRITE_ERR : SDM_IDLE;
            return;
        }
        memcpy(sdm.img + (size_t)sdm.addr * 512, sdm.wbuf, 512);
        sdm.addr++;
        sdm.blocks_written++;
        sdm_put(0x05);                  /* accepted */
        sdm.busy = 5;
        sdm.phase = sdm.multi ? SDM_WRITE_WAIT : SDM_IDLE;
        return;
    }
    if (sdm.cmd_n == 0 && (b & 0xC0) != 0x40)
        return;                         /* no command starts with this */
    sdm.cmd[sdm.cmd_n++] = b;
    if (sdm.cmd_n == 6) {
        sdm.cmd_n = 0;
        sdm_command();
    }
}

static uint8_t sdm_xfer(uint8_t b, int dma)
{
    uint8_t o = 0xFF;
    int programming = !sdm.out_n && sdm.busy > 0;   /* busy: what comes in meanwhile is not heard */
    sdm.xfers++;
    if (sdm.xfers % 64 == 0)
        sdm_ms++;
    if (!sdm.sel || sdm.absent)
        return 0xFF;
    if (sdm.stuck)
        return 0x00;
    if (sdm.out_n) {
        o = sdm.out[sdm.out_h % (int)sizeof sdm.out];
        sdm.out_h = (sdm.out_h + 1) % (int)sizeof sdm.out;
        sdm.out_n--;
    } else if (sdm.busy > 0) {          /* busy: DO held low */
        sdm.busy--;
        o = 0x00;
    }
    if (sdm.phase == SDM_READ && sdm.out_n < 8)
        sdm_read_next();                /* a multi-block read goes on until CMD12 */
    if (!programming)
        sdm_in(b);
    else if (b != 0xFF)
        sdm.busy_talk++;
    if (dma && sdm.dma_bad)
        o ^= 0x01;
    return o;
}

static void sdm_power_cycle(void)        /* put back (or unstuck): the card from power-on */
{
    sdm.idle = 1;
    sdm.crc_on = 0;
    sdm.acmd = 0;
    sdm.phase = SDM_IDLE;
    sdm.out_n = 0;
    sdm.busy = 0;
    sdm.cmd_n = 0;
    sdm.init_left = 3;
}

/* fm1_sd_hw.h's interface */
static void fm1_sd_select(uint32_t on)
{
    if (!sdm.sel && on)
        sdm.cmd_n = 0;
    sdm.sel = (int)on;
}
static void fm1_sd_baud(uint32_t b) { sdm.baud = b; }
static uint8_t fm1_sd_xfer(uint8_t b) { return sdm_xfer(b, 0); }
static int fm1_sd_rx(void *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        ((uint8_t *)p)[i] = sdm_xfer(0xFF, 1);
    return 0;
}
static int fm1_sd_tx(const void *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        sdm_xfer(((const uint8_t *)p)[i], 0);
    return 0;
}
static void fm1_sd_open(uint32_t baud)
{
    sdm.open = 1;
    sdm.opens++;
    sdm.baud = baud;
    sdm.sel = 0;
}
static void fm1_sd_close(uint32_t lcd_baud)
{
    (void)lcd_baud;
    sdm.sel = 0;
    sdm.open = 0;
}
static int fm1_sd_dma_ok(const void *p) { return !((uintptr_t)p & 3u); }
/* the DMA left running: done at once here (the order of the bytes on the wire is what matters) */
static void fm1_sd_rx_start(void *p, uint32_t n) { fm1_sd_rx(p, n); }
static void fm1_sd_tx_start(const void *p, uint32_t n) { fm1_sd_tx(p, n); }
static int fm1_sd_dma_done(void) { return 1; }
static int fm1_sd_dma_end(uint32_t n) { (void)n; return 0; }
/* the interrupt-driven receive (device/sd_ahead.h): the bytes at once, the interrupt left pending for the test to run */
static int sdm_irq_pending;
static void fm1_sd_rx_irq_start(void *p, uint32_t n)
{
    fm1_sd_rx(p, n);
    sdm_irq_pending = 1;
}
static void fm1_sd_rx_irq_ack(void) { sdm_irq_pending = 0; }
static void fm1_sd_rx_irq_stop(void) { sdm_irq_pending = 0; }
