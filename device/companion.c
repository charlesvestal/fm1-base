/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Henri David */
/* Companion services over USB (fm1.toml companion = true: FM1B_COMPANION; docs/COMPANION.md): what a computer's page,
 * phone app or tool needs from any app, in the platform, so it needs no app-specific protocol for it:
 *   0x60  the capability query: the platform services built, the app's name, version and own tags
 *   0x61  files (app.h APP_FILES): the app's named objects listed, read, written (CRC-32, resumable), deleted;
 *         plat_file_*: a file kept in the data area as two copies, for an app that has nowhere else to keep it
 * In the update's SysEx framing (felucca-bsp ota.c: F0 pack7(00 59 cmd len24 body ~sum8(body)) F7), as backup.c and
 * mirror.c; one request at a time, each answered with its cmd.
 *
 * Hooks (the platform's: unity.c on the device, host/plat_host.c in the simulator):
 *   cp_send(p, n)   one whole F0..F7 frame to the computer (may wait for room): 0 = queued
 * comp_cmd(d, n) takes a decoded frame (d: 00 59 cmd len24 body sum, n its length, checked): 1 = the companion's. */
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_000"
#endif
#ifndef APP_COMPANION_EXTRAS
#define APP_COMPANION_EXTRAS ""
#endif
#define CP_HELLO 0x60u
#define CP_VERSION 2u                                 /* 2: the backup's format at the answer's end (0x60) */
#ifndef FM1B_BACKUP_EXT
#define FM1B_BACKUP_EXT "bak"                         /* the backup files' extension (fm1.toml backup_ext: tools/build.py) */
#endif
#define CP_READ_MAX 256u                              /* a read's bytes at most (its answer fits the SysEx ring whole) */
#define CP_WRITE_MAX 512u                             /* a write's (the computer's frame: 640 bytes between F0 and F7) */
#define CP_BODY_MAX (12u + CP_READ_MAX)               /* the longest answer's body */

static int cp_send(const uint8_t *p, uint32_t n);

static uint8_t cp_msg[7u + CP_BODY_MAX] PLAT_POOL;   /* (the pool: RAM the app's .bss does not count) */
static uint8_t cp_wire[2u + ((7u + CP_BODY_MAX) * 8u + 6u) / 7u] PLAT_POOL;
static uint32_t cp_n;                                 /* the answer's body so far (from cp_msg + 6) */

static uint32_t cp_rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static void cp_u8(uint32_t v)
{
    if (cp_n < CP_BODY_MAX)
        cp_msg[6u + cp_n++] = (uint8_t)v;
}
static void cp_u16(uint32_t v) { cp_u8(v), cp_u8(v >> 8); }
static void cp_u32(uint32_t v) { cp_u16(v), cp_u16(v >> 16); }
static void cp_str(const char *s, uint32_t max)       /* s and its 0, max characters at most */
{
    while (s && *s && max--)
        cp_u8((uint8_t)*s++);
    cp_u8(0);
}

static uint32_t cp_pack7(const uint8_t *in, uint32_t n, uint8_t *out)   /* 8-to-7, LSB first (ota.c's) */
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8u;
        while (nb >= 7u) {
            out[o++] = (uint8_t)(acc & 0x7Fu);
            acc >>= 7;
            nb -= 7u;
        }
    }
    if (nb)
        out[o++] = (uint8_t)(acc & 0x7Fu);
    return o;
}

static void cp_begin(void) { cp_n = 0; }
static int cp_end(uint32_t cmd)                       /* the answer (cp_msg's body) framed and sent */
{
    uint32_t i, s = 0, w;
    cp_msg[0] = 0;
    cp_msg[1] = 0x59;
    cp_msg[2] = (uint8_t)cmd;
    cp_msg[3] = (uint8_t)cp_n;
    cp_msg[4] = (uint8_t)(cp_n >> 8);
    cp_msg[5] = 0;
    for (i = 0; i < cp_n; i++)
        s += cp_msg[6u + i];
    cp_msg[6u + cp_n] = (uint8_t)~s;
    cp_wire[0] = 0xF0;
    w = 1u + cp_pack7(cp_msg, 7u + cp_n, cp_wire + 1);
    cp_wire[w++] = 0xF7;
    return cp_send(cp_wire, w);
}

/* ------------------------------------------------- the capability query --- */
enum {
    CP_S_HELLO = 1u << 0, CP_S_FILES = 1u << 1, CP_S_PARAMS = 1u << 2, CP_S_BACKUP = 1u << 3, CP_S_MIRROR = 1u << 4,
    CP_S_READER = 1u << 5, CP_S_BLE = 1u << 6, CP_S_WIFI = 1u << 7, CP_S_UAC_IN = 1u << 8, CP_S_UAC_OUT = 1u << 9,
    CP_S_APP_SYSEX = 1u << 10, CP_S_APP_CMD = 1u << 11, CP_S_CPU2 = 1u << 12, CP_S_USB_HOST = 1u << 13,
    CP_S_BT_AUDIO = 1u << 14
};
static uint32_t cp_services(void)
{
    uint32_t s = CP_S_HELLO | CP_S_READER;
#ifdef APP_FILES
    s |= CP_S_FILES;
#endif
#if defined FM1B_BACKUP && FM1B_BACKUP
    s |= CP_S_BACKUP;
#endif
#if defined FM1B_MIRROR && FM1B_MIRROR
    s |= CP_S_MIRROR;
#endif
#if FM1B_BLE
    s |= CP_S_BLE;
#endif
#if FM1B_WIFI
    s |= CP_S_WIFI;
#endif
#if !defined FM1B_UAC || FM1B_UAC              /* (on by default: device/unity.c; the simulator may not define it) */
    s |= CP_S_UAC_IN;
#if !defined FM1B_UAC_OUT || FM1B_UAC_OUT
    s |= CP_S_UAC_OUT;
#endif
#endif
#ifdef APP_SYSEX
    s |= CP_S_APP_SYSEX;
#endif
#ifdef APP_USB_CMD
    s |= CP_S_APP_CMD;
#endif
#if defined FM1B_CPU2 && FM1B_CPU2
    s |= CP_S_CPU2;
#endif
#if defined FM1B_USB_HOST && FM1B_USB_HOST
    s |= CP_S_USB_HOST;
#endif
#if FM1B_BT_AUDIO
    s |= CP_S_BT_AUDIO;
#endif
    return s;
}
static uint32_t cp_nfiles(void);
static uint32_t cp_nparams(void);
static void cp_hello(void)
{
    cp_begin();
    cp_u8(CP_VERSION);
    cp_u32(cp_services());
    cp_u16(CP_READ_MAX);
    cp_u16(CP_WRITE_MAX);
    cp_u16(cp_nfiles());
    cp_u16(cp_nparams());
    cp_str(FELUCCA_ID, 31);
    cp_str(APP_NAME, 31);
    cp_str(APP_VERSION, 31);
    cp_str(APP_COMPANION_EXTRAS, 120);
#if defined FM1B_BACKUP && FM1B_BACKUP                /* version 2: the backup's format (backup.c's image: magic, version) */
    cp_u32(APP_BACKUP_MAGIC);
    cp_u16(APP_BACKUP_VERSION);
    cp_str(FM1B_BACKUP_EXT, 7);
#else
    cp_u32(0);                                        /* (no backup: magic 0, version 0, no extension) */
    cp_u16(0);
    cp_str("", 7);
#endif
    cp_end(CP_HELLO);
}

static uint32_t cp_crc(uint32_t c, const uint8_t *p, uint32_t n)   /* zlib's crc32(c, p, n): chains */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    c = ~c;
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return ~c;
}
static uint32_t cp_rd32(const uint8_t *p) { return cp_rd16(p) | cp_rd16(p + 2) << 16; }

#ifdef APP_FILES
/* ---------------------------------------------------------------- files --- */
#define CP_FILES 0x61u
enum { CP_OK, CP_BUSY, CP_NONE, CP_DENIED, CP_RANGE, CP_CRC, CP_FLASH, CP_ORDER, CP_BAD, CP_UNKNOWN };
enum { CF_LIST, CF_READ, CF_SUM, CF_BEGIN, CF_DATA, CF_COMMIT, CF_DELETE };
static struct {
    uint8_t on;
    uint16_t i;
    uint32_t size, crc, next, run;                    /* the file's size and CRC-32; the bytes come so far, their CRC */
} cw;
#ifdef APP_FILE_COUNT
static uint32_t cp_nfiles(void) { return app_file_count(); }   /* (the app's own count: its app_file is costly) */
#else
static uint32_t cp_nfiles(void)
{
    plat_file_info_t f;
    uint32_t i = 0;
    while (i < 0xFFFFu && app_file(i, &f) == 0)
        i++;
    return i;
}
#endif
static void cp_head(uint32_t op, uint32_t st, uint32_t i)   /* an answer's start: op, status, index */
{
    cp_begin();
    cp_u8(op);
    cp_u8(st);
    cp_u16(i);
}
static int cp_busy(const plat_file_info_t *f) { return !(f->flags & PLAT_FF_PLAYING) && app_file_busy(); }
static uint32_t cp_file_op(uint32_t op, uint32_t i, const uint8_t *b, uint32_t n, const plat_file_info_t *f)
{
    switch (op) {
    case CF_READ: {                                   /* u32 offset, u16 n -> u32 offset, the bytes */
        uint32_t off, k;
        if (n < 6u)
            return CP_BAD;
        if (!(f->flags & PLAT_FF_READ))
            return CP_DENIED;
        off = cp_rd32(b), k = cp_rd16(b + 4);
        if (off > f->size)
            return CP_RANGE;
        k = k > CP_READ_MAX ? CP_READ_MAX : k;
        k = k > f->size - off ? f->size - off : k;
        cp_u32(off);
        if (k && app_file_read(i, off, cp_msg + 6u + cp_n, k))
            return CP_FLASH;
        cp_n += k;
        return CP_OK;
    }
    case CF_SUM: {                                    /* -> u32 size, u32 CRC-32 (read now) */
        uint32_t off, c = 0;
        if (!(f->flags & PLAT_FF_READ))
            return CP_DENIED;
        for (off = 0; off < f->size; off += CP_READ_MAX) {
            uint32_t k = f->size - off > CP_READ_MAX ? CP_READ_MAX : f->size - off;
            if (app_file_read(i, off, cp_wire, k))   /* (cp_wire: free until the answer is framed) */
                return CP_FLASH;
            c = cp_crc(c, cp_wire, k);
        }
        cp_u32(f->size);
        cp_u32(c);
        return CP_OK;
    }
    case CF_BEGIN: {                                  /* u32 size, u32 CRC-32 -> u32 next */
        uint32_t size, crc;
        if (n < 8u)
            return CP_BAD;
        size = cp_rd32(b), crc = cp_rd32(b + 4);
        if (!(f->flags & PLAT_FF_WRITE))
            return CP_DENIED;
        if (size > f->room)
            return CP_RANGE;
        if (cp_busy(f))
            return CP_BUSY;
        if (!(cw.on && cw.i == i && cw.size == size && cw.crc == crc)) {   /* (the same again: it resumes) */
            cw.on = 0;
            if (app_file_begin(i, size))
                return CP_FLASH;
            cw.on = 1, cw.i = (uint16_t)i, cw.size = size, cw.crc = crc, cw.next = 0, cw.run = 0;
        }
        cp_u32(cw.next);
        return CP_OK;
    }
    case CF_DATA: {                                   /* u32 offset, the bytes -> u32 next */
        uint32_t off, k, skip;
        if (n < 4u)
            return CP_BAD;
        off = cp_rd32(b), k = n - 4u, b += 4;
        if (!cw.on || cw.i != i || off > cw.next) {
            cp_u32(cw.on && cw.i == i ? cw.next : 0u);
            return CP_ORDER;
        }
        if (k > cw.size - off) {
            cp_u32(cw.next);
            return CP_RANGE;
        }
        skip = cw.next - off;                        /* (a frame again, its answer lost: what came is not written twice) */
        if (k > skip) {
            if (cp_busy(f)) {
                cp_u32(cw.next);
                return CP_BUSY;
            }
            if (app_file_write(i, cw.next, b + skip, k - skip)) {
                cw.on = 0;
                cp_u32(0);
                return CP_FLASH;
            }
            cw.run = cp_crc(cw.run, b + skip, k - skip);
            cw.next += k - skip;
        }
        cp_u32(cw.next);
        return CP_OK;
    }
    case CF_COMMIT: {                                 /* -> u32 size, u32 CRC-32 */
        uint32_t rc;
        if (!cw.on || cw.i != i || cw.next != cw.size)
            return CP_ORDER;
        if (cp_busy(f))
            return CP_BUSY;
        cw.on = 0;
        rc = cw.run != cw.crc ? CP_CRC : app_file_commit(i, cw.size, cw.crc) ? CP_FLASH : CP_OK;
        cp_u32(cw.size);
        cp_u32(cw.crc);
        return rc;
    }
    case CF_DELETE:
        if (!(f->flags & PLAT_FF_DELETE))
            return CP_DENIED;
        if (cp_busy(f))
            return CP_BUSY;
        if (cw.on && cw.i == i)
            cw.on = 0;
        return app_file_delete(i) ? CP_FLASH : CP_OK;
    default:
        return CP_UNKNOWN;
    }
}
static void cp_files(const uint8_t *b, uint32_t n)   /* b: the body (op, u16 index, ...) */
{
    plat_file_info_t f = {0};
    uint32_t op = n ? b[0] : 0xFFu, i = n >= 3u ? cp_rd16(b + 1) : 0, st;
    if (n < 3u) {
        cp_head(op, CP_BAD, 0);
        cp_end(CP_FILES);
        return;
    }
    if (app_file(i, &f)) {                            /* (no such file: list says how many there are) */
        cp_head(op, CP_NONE, i);
        if (op == CF_LIST)
            cp_u16(cp_nfiles());
        cp_end(CP_FILES);
        return;
    }
    cp_head(op, CP_OK, i);
    if (op == CF_LIST) {                              /* -> u16 count, u8 kind, u8 flags, u32 size, u32 room, name */
        cp_u16(cp_nfiles());
        cp_u8(f.kind);
        cp_u8(f.flags);
        cp_u32(f.size);
        cp_u32(f.room);
        cp_str(f.name, 31);
        cp_end(CP_FILES);
        return;
    }
    st = cp_file_op(op, i, b + 3, n - 3u, &f);
    cp_msg[7] = (uint8_t)st;
    if (st != CP_OK && op == CF_READ)
        cp_n = 4;                                     /* (a refused read: no offset, no bytes) */
    cp_end(CP_FILES);
}

/* ------------------------------------ files kept in the data area, A / B --- */
#define PF_MAGIC 0x31464D46u                          /* "FMF1" */
typedef struct {
    uint32_t magic, seq, len, crc, hcrc;
} pf_head_t;
static struct {
    uint32_t a, at, end, seq;                         /* the file (its copy A), the copy written, erased up to; its seq */
    uint8_t on;
} pf;
static const pf_head_t *pf_valid(const plat_file_t *f, uint32_t base)
{
    const pf_head_t *h = (const pf_head_t *)plat_data(base);
    if (!h || !PLAT_DATA_OK(base, f->n) || h->magic != PF_MAGIC || h->len > f->n - PLAT_FILE_HEAD ||
        h->hcrc != cp_crc(0, (const uint8_t *)h, 16u))
        return 0;
    return h;
}
static uint32_t pf_current(const plat_file_t *f, const pf_head_t **hp)   /* the current copy's base; 0 none */
{
    const pf_head_t *ha, *hb;
    if (f->n <= PLAT_FILE_HEAD || (f->a | f->b | f->n) & 0xFFFu)
        return 0;
    ha = pf_valid(f, f->a), hb = pf_valid(f, f->b);
    if (ha && (!hb || (int32_t)(ha->seq - hb->seq) > 0)) {
        *hp = ha;
        return f->a;
    }
    *hp = hb;
    return hb ? f->b : 0;
}
int plat_file_size(const plat_file_t *f)
{
    const pf_head_t *h;
    return pf_current(f, &h) ? (int)h->len : -1;
}
const uint8_t *plat_file_data(const plat_file_t *f)
{
    const pf_head_t *h;
    uint32_t base = pf_current(f, &h);
    return base ? plat_data(base + PLAT_FILE_HEAD) : 0;
}
int plat_file_read(const plat_file_t *f, uint32_t off, void *dst, uint32_t n)
{
    const pf_head_t *h;
    uint32_t base = pf_current(f, &h);
    const uint8_t *p;
    if (!base || off > h->len || n > h->len - off || !(p = plat_data(base + PLAT_FILE_HEAD)))
        return -1;
    memcpy(dst, p + off, n);
    return 0;
}
int plat_file_begin(const plat_file_t *f, uint32_t size)
{
    const pf_head_t *h;
    uint32_t cur;
    pf.on = 0;
    if (f->n <= PLAT_FILE_HEAD || size > f->n - PLAT_FILE_HEAD || (f->a | f->b | f->n) & 0xFFFu ||
        !PLAT_DATA_OK(f->a, f->n) || !PLAT_DATA_OK(f->b, f->n))
        return -1;
    cur = pf_current(f, &h);
    pf.a = f->a;
    pf.at = cur == f->a ? f->b : f->a;
    pf.seq = cur ? h->seq + 1u : 1u;
    if (plat_data_erase(pf.at))                       /* its first sector (the head's) now, the others as data comes */
        return -1;
    pf.end = pf.at + 0x1000u;
    pf.on = 1;
    return 0;
}
int plat_file_write(const plat_file_t *f, uint32_t off, const void *src, uint32_t n)
{
    uint32_t at = pf.at + PLAT_FILE_HEAD + off;
    if (!pf.on || pf.a != f->a || off > f->n - PLAT_FILE_HEAD || n > f->n - PLAT_FILE_HEAD - off)
        return -1;
    while (pf.end < at + n) {
        if (plat_data_erase(pf.end))
            return -1;
        pf.end += 0x1000u;
    }
    return n ? plat_data_write(at, src, n) : 0;
}
int plat_file_commit(const plat_file_t *f, uint32_t size, uint32_t crc)
{
    pf_head_t h;
    const uint8_t *p = plat_data(pf.at + PLAT_FILE_HEAD);
    if (!pf.on || pf.a != f->a || !p || size > f->n - PLAT_FILE_HEAD || cp_crc(0, p, size) != crc)
        return -1;                                    /* (read back from the flash: what it holds, checked) */
    pf.on = 0;
    h.magic = PF_MAGIC, h.seq = pf.seq, h.len = size, h.crc = crc;
    h.hcrc = cp_crc(0, (const uint8_t *)&h, 16u);
    return plat_data_write(pf.at, &h, sizeof h) || !pf_valid(f, pf.at) ? -1 : 0;
}
int plat_file_delete(const plat_file_t *f)
{
    if (pf.on && pf.a == f->a)
        pf.on = 0;
    if (pf_valid(f, f->a) && plat_data_erase(f->a))
        return -1;
    if (pf_valid(f, f->b) && plat_data_erase(f->b))
        return -1;
    return 0;
}
#else
static uint32_t cp_nfiles(void) { return 0; }
#endif

static uint32_t cp_nparams(void) { return 0; }

/* a frame in the update's framing (d: decoded, n its length; ota.c checked the header and the sum): 1 = ours */
static int comp_cmd(const uint8_t *d, uint32_t n)
{
    if (n < 7u)
        return 0;
    switch (d[2]) {
    case CP_HELLO:
        cp_hello();
        return 1;
#ifdef APP_FILES
    case CP_FILES:
        cp_files(d + 6, n - 7u);
        return 1;
#endif
    default:
        return 0;
    }
}

#ifdef PLAT_HOST
/* the host simulator and the browser: a raw frame (F0 .. F7) as the device's ota_take decodes and checks it; 1 = ours */
static int comp_sysex(const uint8_t *raw, uint32_t n)
{
    static uint8_t dec[640];
    uint32_t acc = 0, nb = 0, d = 0, i, s = 0;
    if (n < 3u || raw[0] != 0xF0u || raw[n - 1u] != 0xF7u)
        return 0;
    for (i = 1; i + 1u < n; i++) {
        if (raw[i] & 0x80u)
            return 0;
        acc |= (uint32_t)raw[i] << nb;
        nb += 7u;
        if (nb >= 8u) {
            if (d >= sizeof dec)
                return 0;
            dec[d++] = (uint8_t)acc;
            acc >>= 8;
            nb -= 8u;
        }
    }
    if (d < 7u || dec[0] != 0 || dec[1] != 0x59u || (cp_rd16(dec + 3) | (uint32_t)dec[5] << 16) + 7u != d)
        return 0;
    for (i = 6; i + 1u < d; i++)
        s += dec[i];
    if ((uint8_t)~s != dec[d - 1u])
        return 0;
    return comp_cmd(dec, d);
}
#endif
