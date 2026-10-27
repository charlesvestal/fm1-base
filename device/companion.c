/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Henri David */
/* Companion services over USB (fm1.toml companion = true: FM1B_COMPANION; docs/COMPANION.md): what a computer's page,
 * phone app or tool needs from any app, in the platform, so it needs no app-specific protocol for it:
 *   0x60  the capability query: the platform services built, the app's name, version and own tags
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

static uint32_t cp_nfiles(void) { return 0; }
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
