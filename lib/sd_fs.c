/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Julius Vitkauskas */
/* The SD card's files (fm1.toml sd = true: FM1B_SD; plat.h plat_sd_*): FatFs (lib/fatfs: ChaN's, its own licence,
 * LICENSE.txt there; ffconf.h: the settings) on the card layer, the device's (device/sd.c: sd_card_start,
 * sd_card_stop, plat_sd_read / _write / _sectors) or the host's stand-in (host/plat_host.c, an image file). One
 * volume, drive "": the app calls FatFs directly (#include "ff.h": f_open, f_read, f_write, f_lseek, f_opendir ...)
 * once plat_sd_start has mounted it. Included once, by device/unity.c or host/plat_host.c. */
#include "fatfs/ff.h"
#include "fatfs/diskio.h"

static FATFS sd_fs PLAT_POOL;              /* the volume, with its sector window (FF_FS_TINY: the files share it) */
static uint8_t sd_mounted;

DSTATUS disk_status(BYTE pdrv) { return pdrv || plat_sd_sectors() == 0 ? STA_NOINIT : 0; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }   /* the card is plat_sd_start's */

DRESULT disk_read(BYTE pdrv, BYTE *buf, LBA_t sector, UINT n)
{
    if (disk_status(pdrv))
        return RES_NOTRDY;
    return plat_sd_read((uint32_t)sector, buf, n) ? RES_ERROR : RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT n)
{
    if (disk_status(pdrv))
        return RES_NOTRDY;
    return plat_sd_write((uint32_t)sector, buf, n) ? RES_ERROR : RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buf)
{
    if (disk_status(pdrv))
        return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:                        /* nothing held: every write is on the card when it returns */
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buf = plat_sd_sectors();
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buf = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buf = 1;
        return RES_OK;
    }
    return RES_PARERR;
}

int plat_sd_start(void)
{
    int rc;
    if (sd_mounted) {
        f_mount(0, "", 0);
        sd_mounted = 0;
    }
    rc = sd_card_start();
    if (rc)
        return rc;
    if (f_mount(&sd_fs, "", 1) != FR_OK)
        return SD_E_NOFS;
    sd_mounted = 1;
    return 0;
}

void plat_sd_stop(void)
{
    if (sd_mounted)
        f_mount(0, "", 0);
    sd_mounted = 0;
    sd_card_stop();
}

const char *plat_sd_what(int e)
{
    switch (e) {
    case 1: return "ready";
    case 0: return "not started";
    case SD_E_NOCARD: return "no card";
    case SD_E_INIT: return "card did not start";
    case SD_E_CARD: return "card not supported";
    case SD_E_TIMEOUT: return "card not answering";
    case SD_E_CRC: return "data damaged";
    case SD_E_READ: return "read refused";
    case SD_E_WRITE: return "write refused";
    case SD_E_RANGE: return "past the card's end";
    case SD_E_STATE: return "card not started";
    case SD_E_NOFS: return "no FAT file system";
    case SD_E_BUSY: return "busy";
    }
    return "error";
}

#if FM1B_MSC
/* The card as disk mode's USB drive (fm1.toml msc = true): plat_disk_mode(plat_sd_disk()), and plat_sd_disk_poll()
 * in the main loop instead of plat_disk_poll. Disk mode asks for a block at a time and waits on USB between them;
 * meanwhile (plat_disk_t work) the card's next blocks move by DMA in a stream left open (device/sd.c: CMD18 into a
 * ring ahead of the computer's reads, CMD25 out of a queue of its writes), so the card and USB work at once, as
 * m-apps' usbdisk does. A WRITE's blocks are on the card before the computer hears it succeeded (plat_disk_t sync).
 * No streams near the card's end (reading ahead up to it upsets some cards: m-apps), without DMA, or after an error:
 * plain plat_sd_read / _write there, with their retries. The LCD may draw at any time: its lcd_sync ends a stream
 * (sd_bus_release), and the next step opens another */
#ifndef APP_SD_DISK_RA
#define APP_SD_DISK_RA 32u                 /* blocks the read ring holds: 16 KiB of the pool */
#endif
#ifndef APP_SD_DISK_WB
#define APP_SD_DISK_WB 32u                 /* blocks the write queue holds: 16 KiB of the pool */
#endif
#define SDK_CHUNK 8u                       /* blocks a plain read-ahead reads */
#define SDK_NEAR_END 256u
static uint8_t sdk_ra[APP_SD_DISK_RA][512] __attribute__((aligned(4))) PLAT_POOL;   /* block b in slot b % RA */
static uint8_t sdk_wb[APP_SD_DISK_WB][512] __attribute__((aligned(4))) PLAT_POOL;   /* block b in slot b % WB */
static struct {
    uint32_t ra_lba, ra_n;                 /* the ring holds [ra_lba, ra_lba + ra_n); ra_lba: the block last given,
                                            * kept while USB may still be sending it */
    uint8_t r_dma;                         /* block ra_lba + ra_n on its way into its slot (the read stream) */
    uint8_t w_stream;                      /* the write stream (open or closed by the LCD) is the queue's */
    uint32_t wq_lba, wq_n;                 /* the writes not yet on the card: [wq_lba, wq_lba + wq_n) */
    int w_err;                             /* a write failed since the last sync (the WRITE fails) */
    uint8_t w_plain;                       /* a stream's trouble: the rest written plainly (they decide) */
    uint32_t near;                         /* no streams from here on */
    uint32_t r_bad;                        /* plat_ms of a read stream's failure: plain reads (their retries, the
                                            * clock's steps) for a second after */
    uint32_t w_touch;                      /* plat_ms of the last block the computer wrote */
    uint32_t r_touch;                      /* plat_ms of the last block the computer read (read-ahead stops 50 ms
                                            * after: not the command's age, as plat_disk_idle_ms is, since a long
                                            * READ runs well past 50 ms) */
} sdk;

#ifdef SD_NO_STREAMS
/* the host: the streams as plain sector calls (host/plat_host.c, an image file) */
static uint8_t sdh_mode, sdh_wpend;      /* sdh_wpend: a block started, its write at finish */
static uint32_t sdh_next, sdh_wnext;
static uint8_t *sdh_p;
static const uint8_t *sdh_wp;
static int sd_stream_mode(void) { return sdh_mode; }
static uint32_t sd_rs_next(void) { return sdh_next; }
static uint32_t sd_ws_next(void) { return sdh_wnext; }
static int sd_ws_finish(void);
static int sd_stream_close(void)
{
    int rc = sdh_wpend ? sd_ws_finish() : 0;   /* (a block started, written: as the device's close) */
    sdh_mode = 0;
    return rc;
}
static int sd_rs_open(uint32_t lba) { sdh_mode = 1; sdh_next = lba; return 0; }
static int sd_rs_start(uint8_t *p) { sdh_p = p; return 0; }
static int sd_rs_poll(void) { return 1; }
static int sd_rs_finish(void) { return plat_sd_read(sdh_next++, sdh_p, 1); }
static int sd_ws_open(uint32_t lba) { sdh_mode = 2; sdh_wnext = lba; sdh_wpend = 0; return 0; }
static int sd_ws_ready(void) { return !sdh_wpend; }
static void sd_ws_start(const uint8_t *p) { sdh_wp = p; sdh_wpend = 1; }
static int sd_ws_poll(void) { return 1; }
static int sd_ws_finish(void)
{
    sdh_wpend = 0;
    return plat_sd_write(sdh_wnext++, sdh_wp, 1) ? SD_E_WRITE : 0;
}
static int sd_ws_err(void) { return 0; }
#define SDS_READ 1
#define SDS_WRITE 2
#define SD_W_INFLIGHT() (sdh_wpend)
#else
#define SD_W_INFLIGHT() (sds.mode == SDS_WRITE && sds.dma)
#endif

static int sdk_fill(uint32_t want)         /* plain read-ahead: up to want blocks more into the ring; the blocks, -1 */
{
    uint32_t at = sdk.ra_lba + sdk.ra_n, slot = at % APP_SD_DISK_RA, n = want, end = plat_sd_sectors();
    if (n > APP_SD_DISK_RA - sdk.ra_n)
        n = APP_SD_DISK_RA - sdk.ra_n;
    if (n > APP_SD_DISK_RA - slot)
        n = APP_SD_DISK_RA - slot;         /* (the ring's end: the rest next time) */
    if (at >= end)
        return 0;
    if (n > end - at)
        n = end - at;
    if (at + n > sdk.near)                 /* near the end: one block, asked for, at a time */
        n = at < sdk.near ? sdk.near - at : !sdk.ra_n;
    if (!n)
        return 0;
    if (plat_sd_read(at, sdk_ra[slot], n))
        return -1;
    sdk.ra_n += n;
    return (int)n;
}

/* ---- writes: a queue, out by the write stream ---- */
/* Errors: a stream's trouble that leaves the blocks not written in the queue (a block refused, the stream not
 * opened, a stream gone too long) only turns the rest to plain writes (w_plain), which decide: a plain write that
 * fails every try fails the WRITE (w_err). Trouble after blocks were counted written (the card stuck busy after one,
 * a clean run's end refused, a stream ended under the LCD with an error) fails it at once: they may not be there */
static void sdk_w_settle(void)             /* the blocks the stream has written, off the queue */
{
    uint32_t done;
    int e;
    if (!sdk.w_stream)
        return;
    done = sd_ws_next() - sdk.wq_lba;
    if (done <= sdk.wq_n) {
        sdk.wq_lba += done;
        sdk.wq_n -= done;
    }
    e = sd_ws_err();
    if (e && !sdk.w_err)
        sdk.w_err = e;
    if (sd_stream_mode() != SDS_WRITE)
        sdk.w_stream = 0;
}

static void sdk_w_close(void)              /* the write stream ended on purpose: an error then is a real one */
{
    int r;
    if (sd_stream_mode() != SDS_WRITE)
        return;
    if ((r = sd_stream_close()) != 0 && !sdk.w_err)
        sdk.w_err = r;
    sdk_w_settle();
}

static int sdk_w_plain(void)               /* the queue written plainly, a block at a time (with retries) */
{
    while (sdk.wq_n) {
        if (plat_sd_write(sdk.wq_lba, sdk_wb[sdk.wq_lba % APP_SD_DISK_WB], 1)) {
            if (!sdk.w_err)
                sdk.w_err = SD_E_WRITE;
            return -1;
        }
        sdk.wq_lba++;
        sdk.wq_n--;
    }
    return 0;
}

static int sdk_w_step(void)                /* one step of the queue's writing: 1 did something, 0 nothing, < 0 error */
{
    int r;
    sdk_w_settle();
    if (sdk.w_err)
        return -1;
    if (!sdk.wq_n)
        return 0;
    if (sdk.wq_lba >= sdk.near || sdk.w_plain) {   /* the card's end, or after a stream's trouble: plainly */
        sdk_w_close();
        if (sdk.w_err)
            return -1;
        return sdk_w_plain() ? -1 : 1;
    }
    if (sd_stream_mode() != SDS_WRITE || sd_ws_next() != sdk.wq_lba) {   /* (a block in flight is still the head) */
        sdk_w_close();
        if (sdk.w_err)
            return -1;
        if (sd_ws_open(sdk.wq_lba)) {
            sdk.w_plain = 1;
            return 1;
        }
        sdk.w_stream = 1;
        return 1;
    }
    if (SD_W_INFLIGHT()) {
        if (!sd_ws_poll())
            return 0;
        if (sd_ws_finish()) {              /* refused: the stream ended by CMD12, the block still queued */
            sd_ws_err();                   /* (that close's error is this refusal's) */
            sdk.w_plain = 1;
        }
        sdk_w_settle();
        return 1;
    }
    r = sd_ws_ready();
    if (r < 0) {                           /* stuck busy after a block counted written */
        sdk.w_err = r;
        sdk_w_settle();
        return -1;
    }
    if (!r)
        return 0;
    sd_ws_start(sdk_wb[sdk.wq_lba % APP_SD_DISK_WB]);
    return 1;
}

static int sdk_w_drain(void)               /* every write on the card, the stream ended: 0, or -1 a write failed */
{
    int r;
    uint32_t t0 = plat_ms();
    while (sdk.wq_n && sdk_w_step() >= 0)
        if (plat_ms() - t0 > 1500u) {      /* (a DMA that never ends: the stream given up, the rest plainly) */
            sdk.w_plain = 1;
            break;
        }
    sdk_w_close();
    sdk.w_stream = 0;
    if (sdk.wq_n && !sdk.w_err)            /* (what the stream left: plainly) */
        sdk_w_plain();
    r = sdk.w_err || sdk.wq_n ? -1 : 0;
    sdk.w_err = 0;
    sdk.w_plain = 0;
    sdk.wq_n = 0;
    return r;
}

/* ---- reads: a ring, fed by the read stream ---- */
static void sdk_r_stop(void)
{
    if (sd_stream_mode() == SDS_READ)
        sd_stream_close();
    sdk.r_dma = 0;
    sdk.ra_n = 0;
}

static void sdk_r_failed(void) { sdk.r_bad = plat_ms() | 1u; }   /* (0: none) */
static int sdk_r_plain(void)               /* a stream failed lately: plain reads, which retry and slow the clock */
{
    if (sdk.r_bad && plat_ms() - sdk.r_bad >= 1000u)
        sdk.r_bad = 0;
    return sdk.r_bad != 0;
}

static int sdk_r_step(void)                /* one step of read-ahead: 1 did something */
{
    uint32_t at = sdk.ra_lba + sdk.ra_n;
    if (sdk.r_dma) {
        if (sd_stream_mode() != SDS_READ) {   /* ended for the LCD: the block in flight dropped */
            sdk.r_dma = 0;
            return 1;
        }
        if (!sd_rs_poll())
            return 0;
        sdk.r_dma = 0;
        if (!sd_rs_finish())
            sdk.ra_n++;
        else
            sdk_r_failed();
        return 1;
    }
    if (!sdk.ra_n || sdk.ra_n >= APP_SD_DISK_RA || at >= sdk.near || plat_ms() - sdk.r_touch >= 50u)
        return 0;                          /* (none while the computer is quiet) */
    if (sdk_r_plain() || ((sd_stream_mode() != SDS_READ || sd_rs_next() != at) && sd_rs_open(at)))
        return sdk.ra_n + SDK_CHUNK <= APP_SD_DISK_RA && sdk_fill(SDK_CHUNK) > 0;   /* no stream: plainly */
    if (sd_rs_start(sdk_ra[at % APP_SD_DISK_RA])) {
        sdk_r_failed();
        return 1;
    }
    sdk.r_dma = 1;
    return 1;
}

static const uint8_t *sdk_read(uint32_t lba)
{
    sdk.r_touch = plat_ms();
    if (sdk.wq_n || sd_stream_mode() == SDS_WRITE)
        sdk_w_drain();                     /* (its error is the WRITE's: already answered or to be) */
    if (lba - sdk.ra_lba < sdk.ra_n) {     /* in the ring: the blocks before it let go */
        sdk.ra_n -= lba - sdk.ra_lba;
        sdk.ra_lba = lba;
        return sdk_ra[lba % APP_SD_DISK_RA];
    }
    if (sdk.r_dma && lba == sdk.ra_lba + sdk.ra_n && sd_stream_mode() == SDS_READ) {   /* on its way: waited for */
        sdk.r_dma = 0;
        if (!sd_rs_finish()) {
            sdk.ra_n++;
            sdk.ra_n -= lba - sdk.ra_lba;
            sdk.ra_lba = lba;
            return sdk_ra[lba % APP_SD_DISK_RA];
        }
        sdk_r_failed();
    }
    sdk_r_stop();                          /* a miss: from here */
    sdk.ra_lba = lba;
    if (lba < sdk.near && !sdk_r_plain() && !sd_rs_open(lba)) {
        if (!sd_rs_start(sdk_ra[lba % APP_SD_DISK_RA]) && !sd_rs_finish()) {
            sdk.ra_n = 1;
            return sdk_ra[lba % APP_SD_DISK_RA];
        }
        sdk_r_failed();
    }
    if (sdk_fill(lba < sdk.near ? SDK_CHUNK : 1) <= 0)   /* no stream: plainly, with retries */
        return 0;                          /* (disk mode sends zeros: plat_sd_read has tried 4 times) */
    return sdk_ra[lba % APP_SD_DISK_RA];
}

static int sdk_write(uint32_t lba, const uint8_t *b)
{
    uint32_t i, t0;
    sdk.w_touch = plat_ms();
    if (sdk.ra_n || sdk.r_dma)
        sdk_r_stop();                      /* read ahead before this write: stale */
    if (sdk.wq_n && lba != sdk.wq_lba + sdk.wq_n && sdk_w_drain())
        return -1;
    for (t0 = plat_ms(); sdk.wq_n == APP_SD_DISK_WB; )   /* full: the oldest out first (1.5 s at most) */
        if (sdk_w_step() < 0 || plat_ms() - t0 > 1500u)
            break;
    if (sdk.wq_n == APP_SD_DISK_WB)
        return -1;
    if (!sdk.wq_n)
        sdk.wq_lba = lba;
    for (i = 0; i < 512u; i++)
        sdk_wb[lba % APP_SD_DISK_WB][i] = b[i];
    sdk.wq_n++;
    sdk_w_step();                          /* moving */
    return 0;
}

static void sdk_work(void)                 /* while disk mode waits on USB: a step of the writes, else the reads */
{
    if (sdk.wq_n || sd_stream_mode() == SDS_WRITE)
        sdk_w_step();
    else
        sdk_r_step();
}

static int sdk_sync(void) { return sdk_w_drain(); }
static void sdk_flush(void) { sdk_w_drain(); }

static uint32_t sdk_gen;                   /* the card's number as the drive's medium: another each card found */
static int sdk_present(void) { return plat_sd_state() == 1 ? (int)sdk_gen : 0; }

static plat_disk_t sdk_disk = {0, sdk_read, sdk_write, sdk_flush, "fm1-base", "SD card", sdk_sync, sdk_work, 1,
                               sdk_present};
static uint32_t sdk_probe_t;               /* plat_ms of the last look for a card, while it's lost */

static void sdk_reset(void)                /* a card (started), as the drive's medium: nothing held, its size */
{
    uint32_t end = plat_sd_sectors();
    if (++sdk_gen > 0x7FFFFFFFu)
        sdk_gen = 1;
    memset(&sdk, 0, sizeof sdk);
    sdk.near = end > SDK_NEAR_END ? end - SDK_NEAR_END : 0;
    sdk_disk.blocks = end;
}

const plat_disk_t *plat_sd_disk(void)
{
    uint32_t end = plat_sd_sectors();
    if (!end)
        return 0;
    if (sd_mounted)
        f_mount(0, "", 0);                 /* the files are the computer's now */
    sd_mounted = 0;
    sdk_reset();
    return &sdk_disk;
}

int plat_sd_disk_poll(void)
{
    int r = plat_disk_poll(), k;
    if (plat_sd_state() != 1) {            /* the card lost (pulled): looked for every 2 s; the computer hears "no
                                            * medium" meanwhile, then "medium changed" (sdk_present) */
        if (plat_ms() - sdk_probe_t >= 2000u) {
            sdk_probe_t = plat_ms();
            if (!sd_card_start())
                sdk_reset();
        }
        return r;
    }
    if (!sdk.wq_n && plat_ms() - sdk.r_touch >= 1000u && plat_ms() - sdk.w_touch >= 1000u &&
        plat_ms() - sdk_probe_t >= 1000u) {   /* quiet a second: the card still there? (no card-detect line) */
        sdk_probe_t = plat_ms();
        if (!sd_card_check())
            return r;                      /* lost: "no medium" from now (sdk_present) */
    }
    if (r == PLAT_DISK_MOUNTED)            /* between transfers: the streams moved on, a few steps */
        for (k = 0; k < 64; k++) {
            if (sdk.wq_n || sd_stream_mode() == SDS_WRITE) {
                if (sdk_w_step() <= 0)
                    break;
            } else if (sdk_r_step() <= 0)
                break;
        }
    return r;
}
#endif

#include "fatfs/ff.c"
#include "fatfs/ffunicode.c"
