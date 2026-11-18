/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Julius Vitkauskas */
/* A microSD card in SPI mode (fm1.toml sd = true: FM1B_SD; plat.h plat_sd_*): the card layer, on felucca-bsp's
 * fm1_sd_hw.h (the pins and SPI1: the m-card board on the FM-1). The file system on top is lib/sd_fs.c (FatFs).
 *
 * Main loop only, as the screen it shares SPI1 with: every call is one session between two LCD transfers (lcd_sync
 * first, the panel's SPI set back after), and blocks until it is done. A sector at 20 MHz is ~0.3 ms.
 *
 * Start (sd_card_start): 400 kHz; CMD0, CMD8, ACMD41, CMD58 (SDSC v1 and v2, SDHC, SDXC; no MMC); CMD16 512 for
 * SDSC; CRCs on (CMD59): from then on the card checks every command and every block written, and every block read is
 * checked here (CRC16), so a clock too fast or a poor contact shows as an error, never as wrong data. CMD9 gives the
 * size. Then the clock: the fastest of SD_BAUDS (LSB / 3 = 20 MHz at 360 MHz, the most a card takes at default
 * speed, then 15, 10, 5 MHz, then 400 kHz) at which sector 0 reads three times with good CRCs; m-card's series
 * resistors set a ceiling, and its README asks for exactly this. Then DMA: what SPI1 sends while it receives by DMA
 * is not established (the card's DI wants 0xFF there; m-card pulls it up), so reads go by DMA only once 4 reads of
 * two sectors that way came in with good CRCs; else the card is started again and reads go byte by byte (~1 MB/s).
 *
 * A read or write that fails is tried again, up to SD_TRIES times; after each second failure the clock steps down
 * one (sd.step). Errors: plat.h SD_E_*. */

#ifndef SD_TRIES
#define SD_TRIES 4
#endif
#if FM1B_CPU_396
#define SD_LSB_KHZ 66000u
#else
#define SD_LSB_KHZ 60000u                  /* SPI1's source: LSB, 60 MHz at the stock 360 MHz */
#endif
#define SD_BAUD_SLOW ((SD_LSB_KHZ + 399u) / 400u - 1u)          /* 400 kHz at most: the start */
#ifndef FM1B_SD_FINE
#define FM1B_SD_FINE 0
#endif
#if FM1B_SD_FINE                           /* fm1.toml sd_clock_fine: every step SPI1 has between 20 and 5 MHz (20 15 12
                                            * 10 8.57 7.5 6.67 6 5), for a board whose ceiling falls between the four */
static const uint16_t SD_BAUDS[] = {2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 11u, SD_BAUD_SLOW};
#else
static const uint16_t SD_BAUDS[] = {2u, 3u, 5u, 11u, SD_BAUD_SLOW};   /* LSB / (b + 1) */
#endif
#define SD_NBAUDS (sizeof SD_BAUDS / sizeof SD_BAUDS[0])
#define SD_ACMD 0x80u
#define SD_R1_IDLE 0x01u
#define SD_R1_ILLEGAL 0x04u

static struct {
    int8_t state;                          /* 1 started, 0 not, < 0 the last start's error */
    uint8_t hc, ver, dma, step, stepped;   /* block addressing (SDHC / SDXC); 1 / 2; reads by DMA; SD_BAUDS[step] */
    uint32_t sectors, crc_errors, retries;
} sd;
static uint8_t sd_blk[512] __attribute__((aligned(4))) PLAT_POOL;   /* a block when the caller's buffer can't take DMA */

/* A call's time budget: every wait and retry stops at its deadline, so a card that's gone or stuck busy (its DO held
 * low) costs a call SD_CALL_MS at most, not the sum of each step's own limit; with the card then taken for lost
 * (sd.state < 0, plat_sd_start for it again), whatever comes next fails at once. The main loop's watchdog (8 s) is
 * far off either way */
#ifndef SD_CALL_MS
#define SD_CALL_MS 1500u                   /* a read or a write, its retries included */
#endif
#define SD_START_MS 4000u                  /* plat_sd_start: every clock tried */
#define SD_CLOSE_MS 600u                   /* a stream's end */
#define SD_BUSY_MS 600u                    /* a card busy programming: up to 250 ms (SDHC), 500 (SDXC) */
static uint32_t sd_due;                    /* plat_ms deadline of the call running (0: none) */
static int sd_late(void) { return sd_due && (int32_t)(plat_ms() - sd_due) >= 0; }
static uint32_t sd_budget(uint32_t ms)     /* a deadline ms ahead, unless one sooner is set already: the old one */
{
    uint32_t old = sd_due, due = (plat_ms() + ms) | 1u;
    if (!old || (int32_t)(due - old) < 0)
        sd_due = due;
    return old;
}

/* streams (disk mode, lib/sd_fs.c): a multi-block read (CMD18) or write (CMD25) left open across calls, each block
 * moved by DMA while the caller feeds USB. The bus stays the card's meanwhile; the LCD's next transfer (lcd_sync) and
 * any other card call close it first (sd_bus_release) */
enum { SDS_NONE, SDS_READ, SDS_WRITE };
static struct {
    uint8_t mode, dma, busy;               /* busy: a written block being programmed */
    uint8_t wabort;                        /* a written block refused: the write ends by CMD12, not the stop token */
    uint8_t parked;                        /* an app's write between two blocks, its CMD25 left open, the bus lent to
                                            * the LCD (sd_bus_lend) */
    int8_t werr;                           /* a write stream closed by someone else, and how it ended */
    uint16_t crc;                          /* the write in flight's */
    uint32_t next, wnext, busy_t;          /* the read's next block; the write's (every block before it written) */
    uint8_t *p;
} sds;
static void sd_bus_release(void);

static const uint16_t SD_CRC16[256] = {     /* CRC-16/XMODEM (0x1021, 0): the data blocks' */
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
};

static uint16_t sd_crc16(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0;
    while (n--)
        c = (c << 8 ^ SD_CRC16[(c >> 8 ^ *p++) & 0xFFu]) & 0xFFFFu;
    return (uint16_t)c;
}

static uint8_t sd_crc7(const uint8_t *p, uint32_t n)   /* the commands' (x^7 + x^3 + 1), in bits 7..1 */
{
    uint32_t c = 0, i, b;
    for (i = 0; i < n; i++)
        for (b = 0x80u; b; b >>= 1) {
            c <<= 1;
            if ((c & 0x80u) ^ (p[i] & b ? 0x80u : 0u))
                c ^= 0x09u;
        }
    return (uint8_t)(c << 1 & 0xFEu);
}

static int sd_ready(uint32_t ms)           /* the card not busy (its DO high) within ms */
{
    uint32_t t0 = plat_ms();
    do {
        if (fm1_sd_xfer(0xFFu) == 0xFFu)
            return 1;
    } while (plat_ms() - t0 < ms && !sd_late());
    return 0;
}

/* a command, the card left selected for what follows: its R1 (0xFF: none). An ACMD sends CMD55 first, which is no
 * ACMD (tools/stack_depth.py bounds this recursion at 2) */
static uint8_t sd_cmd(uint32_t c, uint32_t arg)
{
    uint8_t f[6], r = 0xFFu;
    uint32_t i;
    if (c & SD_ACMD) {
        r = sd_cmd(55u, 0);
        if (r > SD_R1_IDLE)
            return r;
        c &= ~SD_ACMD;
    }
    if (c != 12u) {                        /* (CMD12 goes in while a multi-block read still streams) */
        fm1_sd_select(0);
        fm1_sd_xfer(0xFFu);
        fm1_sd_select(1);
        if (c && !sd_ready(500u))
            return 0xFFu;
    }
    f[0] = (uint8_t)(0x40u | c);
    f[1] = (uint8_t)(arg >> 24);
    f[2] = (uint8_t)(arg >> 16);
    f[3] = (uint8_t)(arg >> 8);
    f[4] = (uint8_t)arg;
    f[5] = (uint8_t)(sd_crc7(f, 5) | 1u);
    for (i = 0; i < 6; i++)
        fm1_sd_xfer(f[i]);
    if (c == 12u)
        fm1_sd_xfer(0xFFu);                /* CMD12's stuff byte */
    for (i = 0; i < 10 && (r = fm1_sd_xfer(0xFFu)) & 0x80u; i++)
        ;
    return r;
}

/* a data block's n bytes after its start token, to p (by DMA if asked and p allows it, else byte by byte; through
 * sd_blk if DMA is asked for a buffer it can't use), its CRC16 checked */
static int sd_data_body(uint8_t *p, uint32_t n, int dma)
{
    uint32_t i;
    uint8_t *q = p;
    if (dma) {
        if (!fm1_sd_dma_ok(p))
            q = sd_blk;
        if (fm1_sd_rx(q, n))
            return SD_E_TIMEOUT;
    } else
        for (i = 0; i < n; i++)
            p[i] = fm1_sd_xfer(0xFFu);
    i = (uint32_t)fm1_sd_xfer(0xFFu) << 8;
    i |= fm1_sd_xfer(0xFFu);
    if (i != sd_crc16(q, n)) {
        sd.crc_errors++;
        return SD_E_CRC;
    }
    if (q != p)
        memcpy(p, q, n);
    return 0;
}

static int sd_data_in(uint8_t *p, uint32_t n, int dma)   /* one data block in: its start token, then sd_data_body */
{
    uint32_t t0 = plat_ms();
    uint8_t t;
    while ((t = fm1_sd_xfer(0xFFu)) == 0xFFu)
        if (plat_ms() - t0 > 250u || sd_late())   /* (a read takes 100 ms at most) */
            return SD_E_TIMEOUT;
    if (t != 0xFEu)
        return SD_E_READ;                  /* an error token */
    return sd_data_body(p, n, dma);
}

/* CMD12's answer: none, or an error bit, and the card can't be trusted to have stopped (it may stream on) */
static int sd_stop_bad(uint8_t r) { return r == 0xFFu || (r & 0x7Eu); }

static int sd_read_run(uint32_t lba, uint8_t *p, uint32_t n, int dma)
{
    int rc = 0;
    uint32_t a = sd.hc ? lba : lba << 9;
    if (n == 1)
        return sd_cmd(17u, a) ? SD_E_READ : sd_data_in(p, 512u, dma);
    if (sd_cmd(18u, a))
        return SD_E_READ;
    for (; n && !rc; n--, p += 512)
        rc = sd_data_in(p, 512u, dma);
    if (sd_stop_bad(sd_cmd(12u, 0)) && !rc)
        rc = SD_E_READ;                    /* (the caller's retry starts the card again) */
    if (!sd_ready(500u) && !rc)
        rc = SD_E_TIMEOUT;
    return rc;
}

/* one block out: token, 512 bytes (DMA when p allows it), CRC16, the card's data response, its busy time */
static int sd_data_out(const uint8_t *p, uint8_t token)
{
    uint32_t c = sd_crc16(p, 512u), i;
    uint8_t r;
    fm1_sd_xfer(0xFFu);
    fm1_sd_xfer(token);
    if (fm1_sd_dma_ok(p)) {
        if (fm1_sd_tx(p, 512u))
            return SD_E_TIMEOUT;
    } else
        for (i = 0; i < 512u; i++)
            fm1_sd_xfer(p[i]);
    fm1_sd_xfer((uint8_t)(c >> 8));
    fm1_sd_xfer((uint8_t)c);
    r = fm1_sd_xfer(0xFFu) & 0x1Fu;
    if (r != 0x05u)
        return r == 0x0Bu ? SD_E_CRC : SD_E_WRITE;
    return sd_ready(500u) ? 0 : SD_E_TIMEOUT;
}

static int sd_write_run(uint32_t lba, const uint8_t *p, uint32_t n)
{
    int rc = 0;
    uint32_t a = sd.hc ? lba : lba << 9;
    if (n == 1) {
        if (sd_cmd(24u, a))
            return SD_E_WRITE;
        rc = sd_data_out(p, 0xFEu);
    } else {
        sd_cmd(SD_ACMD | 23u, n);          /* the blocks to come, pre-erased (a hint: its answer doesn't matter) */
        if (sd_cmd(25u, a))
            return SD_E_WRITE;
        for (; n && !rc; n--, p += 512)
            rc = sd_data_out(p, 0xFCu);
        if (rc) {                          /* a block refused: the run ended by CMD12, not the stop token (the SD
                                            * spec's SPI mode, the data response token) */
            sd_cmd(12u, 0);
            sd_ready(500u);
        } else {
            fm1_sd_xfer(0xFFu);
            fm1_sd_xfer(0xFDu);            /* stop */
            fm1_sd_xfer(0xFFu);
            if (!sd_ready(500u))
                rc = SD_E_TIMEOUT;
        }
    }
    if (rc)
        return rc;
    if (sd_cmd(13u, 0) || fm1_sd_xfer(0xFFu))   /* the status (R2): the write's errors */
        return SD_E_WRITE;
    return 0;
}

static void sd_slower(void)                /* after a second failure in a row: the clock one step down */
{
    if (++sd.stepped < 2 || sd.step + 1u >= SD_NBAUDS)
        return;
    sd.stepped = 0;
    sd.step++;
}

static void sd_begin(void)
{
    sd_bus_release();                      /* a stream left open: closed */
    lcd_sync();                            /* the panel's DMA done, CS high: SPI1 is free */
    fm1_sd_open(SD_BAUDS[sd.step]);
}

static void sd_end(void) { fm1_sd_close(LCD_BAUD); }

static int sd_init(void)                   /* the card from power-on (or any state) to ready, at 400 kHz */
{
    uint8_t r = 0xFFu, o[16];
    uint32_t i, t0;
    fm1_sd_baud(SD_BAUD_SLOW);
    fm1_sd_select(0);
    for (i = 0; i < 10; i++)
        fm1_sd_xfer(0xFFu);                /* >= 74 clocks, deselected */
    fm1_sd_select(1);                      /* a transfer still open, which a CMD0 doesn't end: the firmware before left
                                            * it so (a restart resets the chip, not the card; an update and
                                            * plat_restart end it first, a crash or a watchdog can't) or a stop was
                                            * refused. Ended here, each step heard only in its state:
                                            *  - a block of a multi-block write half sent: finished by 515 bytes (any
                                            *    bytes sent would be its tail). Its CRC then almost always wrong, so
                                            *    refused; else (1 in 65536) that one sector gets the 0xFF tail, as a
                                            *    write cut by a power loss would leave it. No card power switch here
                                            *    to do better
                                            *  - a multi-block read (heeding CMD12 only: a CMD0 gets its data, 0x00,
                                            *    "did not start") or a write block refused: CMD12, then its busy
                                            *  - between the blocks of a multi-block write: the stop token, then its
                                            *    busy (sent after CMD12's busy: a card programming hears nothing)
                                            * An idle card ignores all three (refuses CMD12); one still in SD mode
                                            * from power-on is ended a frame the 0xFD may start by the 0xFFs after.
                                            * No CMD0 to a card still busy (resetting a card that programs can
                                            * damage it): the start fails, to be tried again */
    for (i = 0; i < 515; i++)
        fm1_sd_xfer(0xFFu);
    sd_cmd(12u, 0);
    sd_ready(SD_BUSY_MS);
    fm1_sd_xfer(0xFDu);
    for (i = 0; i < 8; i++)
        fm1_sd_xfer(0xFFu);
    if (!sd_ready(SD_BUSY_MS))
        return SD_E_TIMEOUT;
    for (i = 0; i < 20 && (r = sd_cmd(0, 0)) != SD_R1_IDLE; i++)
        ;
    if (r != SD_R1_IDLE)
        return r == 0xFFu ? SD_E_NOCARD : SD_E_INIT;
    sd.ver = 1;
    r = sd_cmd(8u, 0x1AAu);                /* 2.7-3.6 V, check pattern AA */
    if (r == SD_R1_IDLE) {
        for (i = 0; i < 4; i++)
            o[i] = fm1_sd_xfer(0xFFu);
        if ((o[2] & 0x0Fu) != 1u || o[3] != 0xAAu)
            return SD_E_CARD;
        sd.ver = 2;
    } else if (!(r & SD_R1_ILLEGAL))
        return SD_E_INIT;
    t0 = plat_ms();
    do
        r = sd_cmd(SD_ACMD | 41u, sd.ver == 2 ? 1u << 30 : 0);
    while (r == SD_R1_IDLE && plat_ms() - t0 < 1000u && !sd_late());
    if (r)
        return r == SD_R1_IDLE ? SD_E_TIMEOUT : SD_E_CARD;   /* (an MMC refuses ACMD41) */
    sd.hc = 0;
    if (sd.ver == 2) {
        if (sd_cmd(58u, 0))
            return SD_E_INIT;
        for (i = 0; i < 4; i++)
            o[i] = fm1_sd_xfer(0xFFu);
        sd.hc = o[0] >> 6 & 1u;            /* CCS */
    }
    if (!sd.hc && sd_cmd(16u, 512u))
        return SD_E_INIT;
    if (sd_cmd(59u, 1u))                   /* CRCs on */
        return SD_E_INIT;
    if (sd_cmd(9u, 0) || sd_data_in(o, 16u, 0))
        return SD_E_INIT;
    if (o[0] >> 6 == 1u)                   /* CSD 2.0: (C_SIZE + 1) x 512 KiB */
        sd.sectors = (((uint32_t)(o[7] & 0x3Fu) << 16 | (uint32_t)o[8] << 8 | o[9]) + 1u) << 10;
    else {                                 /* CSD 1.0: (C_SIZE + 1) << (C_SIZE_MULT + 2 + READ_BL_LEN) bytes */
        uint32_t cs = ((uint32_t)(o[6] & 3u) << 10 | (uint32_t)o[7] << 2 | o[8] >> 6) + 1u;
        uint32_t sh = ((o[9] & 3u) << 1 | o[10] >> 7) + 2u + (o[5] & 0x0Fu);
        sd.sectors = sh >= 9u ? cs << (sh - 9u) : cs >> (9u - sh);
    }
    return sd.sectors ? 0 : SD_E_INIT;
}

static int sd_probe_read(uint32_t lba, int dma, uint32_t *data)   /* one sector read whole (and whether it held data) */
{
    uint32_t i;
    if (sd_read_run(lba, sd_blk, 1, dma))
        return 0;
    for (i = 0; i < 512u; i++)
        if (sd_blk[i] && sd_blk[i] != 0xFFu) {
            ++*data;
            break;
        }
    return 1;
}

static uint32_t sd_le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Reads that prove a clock (as m-apps' card.c): blank sectors prove nothing, as all zeros pass a CRC however they're
 * misread (a DO held low reads as zeros), and most of a card's first sectors are blank. So: the partition table, the
 * first partition's boot sector and 16 sectors from the start of its FAT. A card with no file system passes too (they
 * are then just more checked reads) */
static int sd_probe(int dma)
{
    uint32_t part = 0, rsv, i, data = 0;
    if (!sd_probe_read(0, dma, &data))
        return 0;
    if (sd_blk[510] == 0x55u && sd_blk[511] == 0xAAu && sd_blk[450])
        part = sd_le32(sd_blk + 454);
    if (part >= sd.sectors)
        part = 0;
    if (!sd_probe_read(part, dma, &data))
        return 0;
    rsv = sd_blk[14] | (uint32_t)sd_blk[15] << 8;
    for (i = 0; i < 16u && part + rsv + i < sd.sectors; i++)
        if (!sd_probe_read(part + rsv + i, dma, &data))
            return 0;
    return 1;
}

/* the fastest clock reads come in right at, from the top. A failed step may leave the card in the middle of a
 * transfer it was asked for (a command's answer misread), so it is started again before the next */
static int sd_tune(void)
{
    int rc;
    for (sd.step = 0; sd.step < SD_NBAUDS; sd.step++) {
        if (sd.step && (rc = sd_init()) != 0)
            return rc;
        fm1_sd_baud(SD_BAUDS[sd.step]);
        if (sd_probe(0))
            return 0;
    }
    sd.step = SD_NBAUDS - 1u;
    fm1_sd_baud(SD_BAUDS[sd.step]);
    return SD_E_CRC;
}

static int sd_card_start(void)
{
    int rc;
    uint32_t old = sd_budget(SD_START_MS);
    sd.state = 0;
    sd.dma = 0;
    sd.stepped = 0;
    sd.step = SD_NBAUDS - 1u;
    sd_begin();
    rc = sd_init();
    if (!rc)
        rc = sd_tune();
    if (!rc) {
        sd.dma = (uint8_t)(sd_probe(1) && sd_probe(1));
        if (!sd.dma) {                     /* DMA's reads are not to be trusted: the card again, from the start */
            sd.step = SD_NBAUDS - 1u;
            rc = sd_init();
            if (!rc)
                rc = sd_tune();
        }
    }
    sd_end();
    sd.state = (int8_t)(rc ? rc : 1);
    sd_due = old;
    return rc;
}

/* after a failed transfer: the card started again (it may still be sending or waiting for data it was asked for),
 * at the clock it ran at, one step down after a second failure in a row */
static void sd_recover(void)
{
    sd_slower();
    if (!sd_init())
        fm1_sd_baud(SD_BAUDS[sd.step]);
}

/* ---- streams ---- */
#ifndef SD_TICKS
#define SD_TICKS() fm1_ticks()             /* the fine clock an app's read waits on (plat_sd_read_next's wait_us) */
#define SD_TICKS_PER_US FM1_TICKS_PER_US
#endif
#define SD_SILENT_MS 250u                  /* an app's read with no block this long: SD_E_TIMEOUT (100 ms the spec) */
#ifndef SD_AHEAD_WAIT
#define SD_AHEAD_WAIT() ((void)0)          /* (the host test: its interrupt run here) */
#endif
#ifndef FM1B_SD_AHEAD
#define FM1B_SD_AHEAD 0
#endif
#if FM1B_SD_AHEAD
#include "sd_ahead.h"                      /* an app's read read ahead in SPI1's interrupt (fm1.toml sd_read_ahead) */
#else
#define sd_ahead_on() 0
static void sd_ahead_begin(uint32_t limit) { (void)limit; }
static void sd_ahead_end(void) {}
static int sd_ahead_take(uint8_t *p) { (void)p; return SD_E_STATE; }
#endif
static int sd_stream_mode(void) { return sds.mode; }
static uint32_t sd_rs_next(void) { return sds.next; }
static uint32_t sd_ws_next(void) { return sds.wnext; }

/* the open stream ended: a read's block in flight dropped, CMD12; a write's block in flight finished, the stop token,
 * its status (CMD13). 0, or the write's error. The bus back to the panel */
static int sd_stream_close(void)
{
    int rc = 0, mode = sds.mode;
    uint8_t r;
    uint32_t old;
    if (mode == SDS_NONE)
        return 0;
    old = sd_budget(SD_CLOSE_MS);
    sds.mode = SDS_NONE;
    if (mode == SDS_READ) {
        sd_ahead_end();                    /* (an app's read ahead stopped first: the bus the polled path's) */
        if (sds.dma) {
            fm1_sd_dma_end(512u);
            fm1_sd_xfer(0xFFu);
            fm1_sd_xfer(0xFFu);
        }
        sds.dma = 0;
        if (sd_stop_bad(sd_cmd(12u, 0))) {  /* not stopped for sure: started again before the bus goes back */
            rc = SD_E_READ;
            sd_recover();
        } else if (!sd_ready(500u))
            rc = SD_E_TIMEOUT;
    } else {
        if (sds.dma) {                     /* the block in flight, finished */
            if (sds.dma == 1 && fm1_sd_dma_end(512u))
                rc = SD_E_TIMEOUT;
            sds.dma = 0;
            fm1_sd_xfer((uint8_t)(sds.crc >> 8));
            fm1_sd_xfer((uint8_t)sds.crc);
            r = fm1_sd_xfer(0xFFu) & 0x1Fu;
            if (!rc && r != 0x05u) {
                rc = r == 0x0Bu ? SD_E_CRC : SD_E_WRITE;
                sds.wabort = 1;
            }
            if (!rc) {
                sds.wnext++;
                sds.busy = 1;
            }
        }
        if (sds.wabort) {                  /* a block refused: CMD12, not the stop token */
            sds.busy = 0;
            sd_cmd(12u, 0);
            sd_ready(500u);
            if (!rc)
                rc = SD_E_WRITE;
        } else {
            if (sds.busy && !sd_ready(500u) && !rc)
                rc = SD_E_TIMEOUT;
            sds.busy = 0;
            fm1_sd_xfer(0xFFu);
            fm1_sd_xfer(0xFDu);            /* stop */
            fm1_sd_xfer(0xFFu);
            if (!sd_ready(500u) && !rc)
                rc = SD_E_TIMEOUT;
        }
        sds.wabort = 0;
        if (!rc && (sd_cmd(13u, 0) || fm1_sd_xfer(0xFFu)))
            rc = SD_E_WRITE;
    }
    sd_end();
    sd_due = old;
    return rc;
}

static int sd_stream_fail(int rc)
{
    sd_stream_close();
    return rc;
}

/* A stream's end for another user of the bus, through a pointer set when a stream opens: a build whose app opens
 * none (the card unused, or plain calls only) links none of the closing below into lcd_sync */
static void sd_streams_end(int lend);
static void (*volatile sd_streams)(int lend);
static void sd_bus_release(void)           /* sd_begin; main_fm1.c before an update: ended, a write's error kept for
                                            * its owner (sds.werr) */
{
    if (sd_streams)
        sd_streams(0);
}
static void sd_bus_lend(void)              /* the LCD (lcd_sync): ended, an app's write parked */
{
    if (sd_streams)
        sd_streams(1);
}

static int sd_rs_open(uint32_t lba)        /* a read from lba on (only with DMA: else SD_E_STATE) */
{
    sd_bus_release();
    if (sd.state != 1 || !sd.dma)
        return SD_E_STATE;
    sd_begin();
    if (sd_cmd(18u, sd.hc ? lba : lba << 9)) {
        sd_end();
        return SD_E_READ;
    }
    sds.mode = SDS_READ;
    sd_streams = sd_streams_end;
    sds.next = lba;
    sds.dma = 0;
    return 0;
}

static int sd_rs_start(uint8_t *p)         /* the next block's start token, then its DMA to p (4-aligned main RAM) */
{
    uint32_t t0 = plat_ms();
    uint8_t t;
    while ((t = fm1_sd_xfer(0xFFu)) == 0xFFu)
        if (plat_ms() - t0 > 250u || sd_late())
            return sd_stream_fail(SD_E_TIMEOUT);
    if (t != 0xFEu)
        return sd_stream_fail(SD_E_READ);
    sds.p = p;
    fm1_sd_rx_start(p, 512u);
    sds.dma = 1;
    return 0;
}

static int sd_rs_poll(void) { return !sds.dma || fm1_sd_dma_done(); }

static int sd_rs_finish(void)              /* the block in flight: its DMA waited for, its CRC checked */
{
    uint32_t c;
    if (sds.mode != SDS_READ || !sds.dma)
        return SD_E_STATE;
    sds.dma = 0;
    if (fm1_sd_dma_end(512u))
        return sd_stream_fail(SD_E_TIMEOUT);
    c = (uint32_t)fm1_sd_xfer(0xFFu) << 8;
    c |= fm1_sd_xfer(0xFFu);
    if (c != sd_crc16(sds.p, 512u)) {
        sd.crc_errors++;
        return sd_stream_fail(SD_E_CRC);
    }
    sds.next++;
    return 0;
}

static int sd_ws_open(uint32_t lba)        /* a write from lba on */
{
    sd_bus_release();
    if (sd.state != 1)
        return SD_E_STATE;
    sd_begin();
    if (sd_cmd(25u, sd.hc ? lba : lba << 9)) {
        sd_end();
        return SD_E_WRITE;
    }
    sds.mode = SDS_WRITE;
    sd_streams = sd_streams_end;
    sds.wnext = lba;
    sds.dma = 0;
    sds.busy = 0;
    return 0;
}

static int sd_ws_ready(void)               /* 1: the next block may start; 0: not yet; < 0: the card stuck busy */
{
    if (sds.mode != SDS_WRITE)
        return SD_E_STATE;
    if (sds.dma)
        return 0;
    if (sds.busy) {
        if (fm1_sd_xfer(0xFFu) != 0xFFu)
            return plat_ms() - sds.busy_t > 500u || sd_late() ? sd_stream_fail(SD_E_TIMEOUT) : 0;
        sds.busy = 0;
    }
    return 1;
}

static void sd_ws_start(const uint8_t *p)  /* the next block: its token, its DMA (its CRC worked out meanwhile) */
{
    uint32_t i;
    fm1_sd_xfer(0xFFu);
    fm1_sd_xfer(0xFCu);
    if (fm1_sd_dma_ok(p)) {
        fm1_sd_tx_start(p, 512u);
        sds.dma = 1;
    } else {
        for (i = 0; i < 512u; i++)
            fm1_sd_xfer(p[i]);
        sds.dma = 2;                       /* (sent byte by byte: done) */
    }
    sds.crc = sd_crc16(p, 512u);
}

static int sd_ws_poll(void) { return sds.dma != 1 || fm1_sd_dma_done(); }

static int sd_ws_finish(void)              /* the block in flight: DMA, CRC, the card's answer; then it programs */
{
    uint8_t r;
    if (sds.mode != SDS_WRITE || !sds.dma)
        return SD_E_STATE;
    if (sds.dma == 1 && fm1_sd_dma_end(512u)) {
        sds.dma = 0;
        return sd_stream_fail(SD_E_TIMEOUT);
    }
    sds.dma = 0;
    fm1_sd_xfer((uint8_t)(sds.crc >> 8));
    fm1_sd_xfer((uint8_t)sds.crc);
    r = fm1_sd_xfer(0xFFu) & 0x1Fu;
    if (r != 0x05u) {
        sds.wabort = 1;                    /* (the close ends it by CMD12) */
        return sd_stream_fail(r == 0x0Bu ? SD_E_CRC : SD_E_WRITE);
    }
    sds.wnext++;
    sds.busy = 1;
    sds.busy_t = plat_ms();
    return 0;
}

static int sd_ws_err(void)                 /* a write stream someone else closed: how it ended (once) */
{
    int e = sds.werr;
    sds.werr = 0;
    return e;
}

/* a read or write that failed every try: the card taken for lost (gone, stuck, or past what retries and a slower
 * clock can mend), so that what comes next fails at once instead of each waiting out its own budget (disk mode asks
 * for hundreds of blocks): plat_sd_start finds it again */
static void sd_lost(int rc)
{
    if (rc && rc != SD_E_RANGE && rc != SD_E_STATE) {
        sd_bus_release();
        sd.state = (int8_t)rc;
    }
}

int plat_sd_read(uint32_t lba, void *buf, uint32_t n)
{
    int rc = 0, t;
    uint32_t old;
    fm1_wdt_feed();                        /* (one FatFs call may be thousands of these: a big card's free space, a
                                            * long file's cluster map. Each is bounded by its budget) */
    if (sd.state != 1)
        return SD_E_STATE;
    if (lba >= sd.sectors || n > sd.sectors - lba)
        return SD_E_RANGE;
    if (!n)
        return 0;
    old = sd_budget(SD_CALL_MS);
    sd_begin();
    for (t = 0; t < SD_TRIES && !sd_late(); t++) {
        rc = sd_read_run(lba, (uint8_t *)buf, n, sd.dma);
        if (!rc) {
            sd.stepped = 0;
            break;
        }
        sd.retries++;
        sd_recover();
    }
    if (rc && !t)
        rc = SD_E_TIMEOUT;                 /* (no time left to try at all) */
    sd_end();
    sd_lost(rc);
    sd_due = old;
    return rc;
}

int plat_sd_write(uint32_t lba, const void *buf, uint32_t n)
{
    int rc = 0, t;
    uint32_t old;
    fm1_wdt_feed();
    if (sd.state != 1)
        return SD_E_STATE;
    if (lba >= sd.sectors || n > sd.sectors - lba)
        return SD_E_RANGE;
    if (!n)
        return 0;
    old = sd_budget(SD_CALL_MS);
    sd_begin();
    for (t = 0; t < SD_TRIES && !sd_late(); t++) {
        rc = sd_write_run(lba, (const uint8_t *)buf, n);
        if (!rc) {
            sd.stepped = 0;
            break;
        }
        sd.retries++;
        sd_recover();
    }
    if (rc && !t)
        rc = SD_E_TIMEOUT;                 /* (no time left to try at all) */
    sd_end();
    sd_lost(rc);
    sd_due = old;
    return rc;
}

/* ---- an app's streams (plat.h plat_sd_read_open ... plat_sd_close) ----
 * On the streams above, one at a time. A read's wait for its next block is the caller's (wait_us, on the chip's fine
 * clock), never longer: SD_E_BUSY, the bus kept the card's. The LCD (lcd_sync: sd_bus_lend) ends a read, which the
 * next plat_sd_read_next opens again where it was; a write it keeps (parked): its last block's busy waited for, the
 * card deselected with its CMD25 open (the state sd_init ends after a restart), so a recording goes on as the screen
 * draws, without a stop and a new CMD25 every frame. Any other card call ends either first (sd_begin) */
static struct {
    uint8_t mode;                          /* SDS_READ, SDS_WRITE: the app's stream (sds.mode: whether it's open) */
    uint8_t planned;                       /* a read of a known length (plat_sd_read_open's n) */
    uint8_t waiting;                       /* a read's wait for its next block begun, at t0 */
    uint32_t next, left;                   /* a read: the next block; the blocks planned still to come */
    uint32_t t0;                           /* plat_ms the wait for the read's next block began */
} sda;

static void sd_unpark(void)                /* a parked write's session again (the LCD's transfer finished first) */
{
    lcd_sync();                            /* (sd_bus_lend: nothing, it's parked) */
    sds.parked = 0;
    fm1_sd_open(SD_BAUDS[sd.step]);
    fm1_sd_select(1);
}

static void sd_streams_end(int lend)       /* sd_bus_release (lend 0), sd_bus_lend (1) */
{
    int w, rc;
    if (sds.parked) {
        if (lend)
            return;
        sd_unpark();
    }
    if (lend && sds.mode == SDS_WRITE && sda.mode == SDS_WRITE && !sds.dma && !sds.wabort &&
        (!sds.busy || sd_ready(SD_BUSY_MS))) {   /* (a busy card selected between the LCD's transfers would hold its
                                                  * DO, the panel's D/C, low against it) */
        sds.busy = 0;
        sds.parked = 1;
        sd_end();
        return;
    }
    if (sds.mode) {
        w = sds.mode == SDS_WRITE;
        rc = sd_stream_close();
        if (w && rc && !sds.werr)
            sds.werr = (int8_t)rc;
    }
}

static int sd_app_open(void)               /* the app's stream open (again) on the card: 0 or SD_E_* */
{
    uint32_t old = sd_budget(SD_CALL_MS);
    int rc = 0;
    if (sda.mode == SDS_WRITE) {
        rc = sd_ws_open(sds.wnext);
    } else {
        sd_bus_release();
        sd_begin();
        if (sd_cmd(18u, sd.hc ? sda.next : sda.next << 9)) {
            sd_end();
            rc = SD_E_READ;
        } else {
            sds.mode = SDS_READ;
            sd_streams = sd_streams_end;
            sds.next = sda.next;
            sds.dma = 0;
            sd_ahead_begin(sda.planned ? sda.left : sd.sectors - sda.next);
        }
    }
    sd_due = old;
    return rc;
}

static int sd_app_fail(int rc)             /* the app's stream over (the card's side ended already): rc */
{
    sd_bus_release();
    sda.mode = SDS_NONE;
    return rc;
}

int plat_sd_read_open(uint32_t sector, uint32_t n)
{
    int rc;
    plat_sd_close();
    if (sd.state != 1)
        return SD_E_STATE;
    if (sector >= sd.sectors || n > sd.sectors - sector)
        return SD_E_RANGE;
    sda.mode = SDS_READ;
    sda.next = sector;
    sda.planned = n != 0;
    sda.left = n;
    sda.waiting = 0;
    rc = sd_app_open();
    if (rc)
        sda.mode = SDS_NONE;
    return rc;
}

static int sd_app_got(void)                /* an app's read: its block in */
{
    sda.waiting = 0;
    sd.stepped = 0;
    sds.next++;
    sda.next++;
    if (sda.planned)
        sda.left--;
    return 0;
}

static int sd_app_bad(int rc)              /* an app's read: its block damaged or refused: the stream ended (the block
                                            * read again at the next call), a step slower after a second in a row */
{
    sda.waiting = 0;
    sd.retries++;
    sd_slower();
    sd_bus_release();
    return rc;
}

static int sd_ahead_next(uint8_t *p, uint32_t t0, uint32_t wait_us)   /* plat_sd_read_next, read ahead */
{
    int rc;
    do {
        rc = sd_ahead_take(p);
        if (!rc)
            return sd_app_got();
        if (rc == SD_E_CRC)
            return sd_app_bad(rc);
        if (rc != SD_E_BUSY)               /* (an error token, or the card silent: the engine's own SD_SILENT_MS) */
            return plat_ms() - sda.t0 >= SD_SILENT_MS ? sd_app_fail(SD_E_TIMEOUT) : sd_app_bad(SD_E_READ);
        SD_AHEAD_WAIT();
    } while (SD_TICKS() - t0 < wait_us * SD_TICKS_PER_US);
    if (plat_ms() - sda.t0 >= SD_SILENT_MS + 20u)   /* (the engine itself silent) */
        return sd_app_fail(SD_E_TIMEOUT);
    return SD_E_BUSY;
}

int plat_sd_read_next(void *buf, uint32_t wait_us)
{
    uint32_t t0 = SD_TICKS();
    uint8_t t;
    int rc;
    if (sda.mode != SDS_READ)
        return SD_E_STATE;
    if (sd.state != 1)
        return sd_app_fail(SD_E_STATE);
    if (sda.next >= sd.sectors || (sda.planned && !sda.left))
        return SD_E_RANGE;                 /* (past the card's end, or the run planned) */
    if (sds.mode != SDS_READ && (rc = sd_app_open()) != 0)
        return sd_app_fail(rc);
    if (!sda.waiting) {
        sda.waiting = 1;
        sda.t0 = plat_ms();
    }
    if (sd_ahead_on())
        return sd_ahead_next((uint8_t *)buf, t0, wait_us);
    do {
        t = fm1_sd_xfer(0xFFu);
        if (t == 0xFEu) {
            rc = sd_data_body((uint8_t *)buf, 512u, sd.dma);
            return rc ? sd_app_bad(rc) : sd_app_got();
        }
        if (t != 0xFFu)                    /* an error token */
            return sd_app_bad(SD_E_READ);
    } while (SD_TICKS() - t0 < wait_us * SD_TICKS_PER_US);
    if (plat_ms() - sda.t0 >= SD_SILENT_MS) {
        sda.waiting = 0;
        return sd_app_fail(SD_E_TIMEOUT);
    }
    return SD_E_BUSY;
}

int plat_sd_write_open(uint32_t sector)
{
    plat_sd_close();
    if (sd.state != 1)
        return SD_E_STATE;
    if (sector >= sd.sectors)
        return SD_E_RANGE;
    sda.mode = SDS_WRITE;
    sds.wnext = sector;
    if (sd_app_open()) {
        sda.mode = SDS_NONE;
        return SD_E_WRITE;
    }
    return 0;
}

int plat_sd_write_next(const void *buf)
{
    int rc;
    if (sda.mode != SDS_WRITE)
        return SD_E_STATE;
    if (sd.state != 1)
        return sd_app_fail(SD_E_STATE);
    if (sds.werr)                          /* ended by another call with an error (sd_bus_release) */
        return sd_app_fail(sd_ws_err());
    if (sds.wnext >= sd.sectors)
        return sd_app_fail(SD_E_RANGE);
    if (sds.parked)
        sd_unpark();
    else if (sds.mode != SDS_WRITE && (rc = sd_app_open()) != 0)   /* (ended cleanly by another call) */
        return sd_app_fail(rc);
    rc = sd_ws_ready();
    if (rc < 0)
        return sd_app_fail(rc);
    if (!rc)
        return SD_E_BUSY;
    if (!buf)
        return 0;
    sd_ws_start((const uint8_t *)buf);
    rc = sd_ws_finish();                   /* (its DMA waited for: ~0.2 ms at 20 MHz; its busy is the next call's) */
    if (rc)
        sda.mode = SDS_NONE;               /* (refused: the stream ended by CMD12; the app knows which block) */
    return rc;
}

int plat_sd_close(void)
{
    int rc = 0, mode = sda.mode;
    if (mode == SDS_NONE)
        return 0;
    sda.mode = SDS_NONE;
    if (sds.parked)
        sd_unpark();
    if (sds.mode == mode)
        rc = sd_stream_close();
    else if (mode == SDS_WRITE && sds.werr)
        rc = sd_ws_err();
    return rc;
}

/* is the card still there? There's no card-detect line: its status (CMD13), asked again after a restart. 1 yes; else
 * it's taken for lost (sd_lost). Main loop, between transfers: disk mode's poll asks every second while the computer
 * is quiet, so a card pulled then is noticed then, not at the next transfer */
static int sd_card_check(void)
{
    int ok;
    uint32_t old;
    if (sd.state != 1)
        return 0;
    old = sd_budget(SD_CALL_MS);
    sd_begin();
    ok = !sd_cmd(13u, 0) && !fm1_sd_xfer(0xFFu);
    if (!ok) {
        sd_recover();
        ok = !sd_cmd(13u, 0) && !fm1_sd_xfer(0xFFu);
    }
    sd_end();
    sd_due = old;
    if (!ok)
        sd_lost(SD_E_NOCARD);
    return ok;
}

uint32_t plat_sd_sectors(void) { return sd.state == 1 ? sd.sectors : 0; }

void plat_sd_info(plat_sd_info_t *i)
{
    i->sectors = plat_sd_sectors();
    i->khz = sd.state == 1 ? SD_LSB_KHZ / (SD_BAUDS[sd.step] + 1u) : 0;
    i->crc_errors = sd.crc_errors;
    i->retries = sd.retries;
    i->timeouts = fm1_sd_timeouts;
    i->hc = sd.hc;
    i->dma = sd.dma;
    i->ver = sd.ver;
}

static void sd_card_stop(void)
{
    sd_bus_release();
    sd.state = 0;
}

int plat_sd_state(void) { return sd.state; }
