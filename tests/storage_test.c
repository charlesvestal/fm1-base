/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Host test of device/storage.c against a simulated NOR flash:
 * erase -> 0xFF, program can only clear bits, page writes must not wrap. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint8_t nor[0x100000];
static int fail_after = -1;            /* torn-write injection: stop after N programs */

static int read_after = -1;
static unsigned erase_calls, program_calls;
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    if (!read_after) return -1;
    if (read_after > 0) read_after--;
    memcpy(dst, nor + off, n); return 0;
}
static int st_erase(uint32_t off) { erase_calls++; memset(nor + off, 0xFF, 4096); return 0; }
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    program_calls++;
    const uint8_t *s = src;
    uint32_t i;
    if (fail_after == 0)
        return -9;
    if (fail_after > 0)
        fail_after--;
    if ((off & 0xFFu) + n > 256u) {
        printf("page wrap at %#x\n", off);
        exit(1);
    }
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    return 0;
}
#include "../include/plat.h"
enum { OBJ_PROJ, OBJ_PANEL, OBJ_NOBJ };       /* an app's object map (app.h): the template's two objects */
#define APP_STORE_MAP {0xFC000u, 0xDC000u}
#include "../device/storage.c"
#define OBJ_PROJECT0 OBJ_PROJ
#define OBJ_SETTINGS OBJ_PANEL

static int check(const char *what, int ok)
{
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static int read_io_test(void)
{
    static uint8_t saved[8192];
    int bad = 0;
    for (unsigned bank = 0; bank < 2; bank++) {
        memset(nor, 255, sizeof nor);
        bad += check("read-fault fixture saves A", st_save(OBJ_PANEL, "old", 3) == 0);
        if (bank) bad += check("read-fault fixture saves B", st_save(OBJ_PANEL, "old", 3) == 0);
        memcpy(saved, nor + st_sector(OBJ_PANEL, 0), sizeof saved);
        unsigned e = erase_calls, p = program_calls;
        for (int point = 0; point < 3; point++) {
            st_hdr_t h;
            read_after = point;
            bad += check("I/O differs from blank/invalid", st_current(OBJ_PANEL, &h) == -2);
            read_after = point;
            int rc = st_save(OBJ_PANEL, "new", 3);
            read_after = -1;
            bad += check("I/O refuses before any erase/program", rc == -2 && erase_calls == e && program_calls == p &&
                         !memcmp(saved, nor + st_sector(OBJ_PANEL, 0), sizeof saved));
        }
        bad += check("read fault cleared -> retry saves", st_save(OBJ_PANEL, "new", 3) == 0);
        /* Winner CRC is invalid: I/O reading the fallback is still fatal. */
        nor[st_sector(OBJ_PANEL, bank ^ 1u) + ST_PAYLOAD_OFF] ^= 1;
        memcpy(saved, nor + st_sector(OBJ_PANEL, 0), sizeof saved);
        e = erase_calls; p = program_calls; read_after = 3;
        int rc = st_save(OBJ_PANEL, "retry", 5);
        read_after = -1;
        bad += check("fallback payload I/O refuses without writes", rc == -2 && erase_calls == e && program_calls == p &&
                     !memcmp(saved, nor + st_sector(OBJ_PANEL, 0), sizeof saved));
        /* Invalid data, unlike I/O, permits recovery using the older copy. */
        bad += check("CRC-invalid newest copy permits save", st_save(OBJ_PANEL, "retry", 5) == 0);
    }
    memset(nor, 0, sizeof nor);
    bad += check("two invalid headers permit a fresh save", st_save(OBJ_PANEL, "new", 3) == 0);
    return bad;
}

int main(void)
{
    char a[600], b[600], got[600];
    int bad = 0, n;
    memset(nor, 0xFF, sizeof nor);
    memset(a, 'A', sizeof a);
    memset(b, 'B', sizeof b);
    bad += check("empty flash loads nothing", st_load(OBJ_PROJECT0, got, sizeof got) < 0);
    bad += check("save A", st_save(OBJ_PROJECT0, a, sizeof a) == 0);
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("load returns A", n == (int)sizeof a && !memcmp(got, a, sizeof a));
    bad += check("save B", st_save(OBJ_PROJECT0, b, sizeof b) == 0);
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("load returns B (newer seq)", n == (int)sizeof b && !memcmp(got, b, sizeof b));
    fail_after = 1;                                  /* payload page 1 written, the rest torn */
    st_save(OBJ_PROJECT0, a, sizeof a);
    fail_after = -1;
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("torn save keeps B", n == (int)sizeof b && !memcmp(got, b, sizeof b));
    fail_after = 3;                                  /* all payload pages, header torn */
    st_save(OBJ_PROJECT0, a, sizeof a);
    fail_after = -1;
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("header not written keeps B", n == (int)sizeof b && !memcmp(got, b, sizeof b));
    bad += check("save A again", st_save(OBJ_PROJECT0, a, sizeof a) == 0);
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("load returns A", n == (int)sizeof a && !memcmp(got, a, sizeof a));
    nor[st_sector(OBJ_PROJECT0, 0) + ST_PAYLOAD_OFF + 10] ^= 0x01;   /* bit rot in one copy */
    nor[st_sector(OBJ_PROJECT0, 1) + ST_PAYLOAD_OFF + 10] ^= 0x01;
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("both copies corrupt -> nothing", n < 0);
    bad += check("other objects untouched", st_load(OBJ_PANEL, got, sizeof got) < 0);
    bad += check("settings save/load",
                 st_save(OBJ_SETTINGS, "hello", 5) == 0 && st_load(OBJ_SETTINGS, got, 5) == 5 && !memcmp(got, "hello", 5));
    bad += check("CRC-32 is zlib's (check value 0xCBF43926)", st_crc32("123456789", 9) == 0xCBF43926u);
    memset(nor, 0xFF, sizeof nor);
    st_save(OBJ_PANEL, a, sizeof a);
    st_save(OBJ_PANEL, b, sizeof b);              /* B is newer, in the other copy */
    {
        st_hdr_t h;
        int cur = st_current(OBJ_PANEL, &h);
        nor[st_sector(OBJ_PANEL, (uint32_t)cur) + ST_PAYLOAD_OFF + 300] ^= 0x10;   /* rot in the newer copy */
    }
    n = st_load(OBJ_PANEL, got, sizeof got);
    bad += check("newer copy rotten -> the older one loads", n == (int)sizeof a && !memcmp(got, a, sizeof a));
    bad += check("save over the rotten copy", st_save(OBJ_PANEL, b, sizeof b) == 0);
    n = st_load(OBJ_PANEL, got, sizeof got);
    bad += check("... loads the new data", n == (int)sizeof b && !memcmp(got, b, sizeof b));
    nor[st_sector(OBJ_PANEL, 0) + 8] ^= 0x01;    /* both headers broken */
    nor[st_sector(OBJ_PANEL, 1) + 8] ^= 0x01;
    bad += check("both headers broken -> nothing", st_load(OBJ_PANEL, got, sizeof got) < 0);
    {   /* the map: every object in a data region, none in the sample slots (0xA0000..0xDBFFF), no overlaps */
        uint32_t o, p, ok = 1;
        for (o = 0; o < OBJ_NOBJ; o++) {
            uint32_t a0 = st_sector(o, 0), a1 = st_sector(o, 1) + 4096;
            int in_data = (a0 >= 0x97000 && a1 <= 0xA0000) || (a0 >= 0xDC000 && a1 <= 0xE0000) ||
                          (a0 >= 0xFC000 && a1 <= 0xFF000);
            if (!in_data || a1 - a0 != 8192)
                ok = 0;
            for (p = 0; p < o; p++)
                if (a0 < st_sector(p, 1) + 4096 && st_sector(p, 0) < a1)
                    ok = 0;
        }
        bad += check("objects in data regions, clear of the sample slots", (int)ok);
    }
    bad += read_io_test();
    printf("%s\n", bad ? "STORAGE TEST FAILED" : "storage test passed");
    return bad != 0;
}
