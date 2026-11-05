/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The SMK-37's flash map (felucca-bsp boards/smk37/fm1_flash.h) enumerated against the stock layout's protected
 * ranges and the app area: every window of this firmware (the data area, the two more, the autosave, the update's
 * staging, the settings pair, the stock marker sector) and the template's store objects on the SMK must be sector
 * aligned, inside the 1 MiB part, above the app area, clear of the ranges never written, and clear of each other;
 * the update loader's windows must agree. The header's own _Static_asserts compile here too.
 *   cc -std=c99 -Ibsp/boards/smk37 -Iinclude tests/smk37_flash_test.c */
#include <stdio.h>
#include <stdint.h>
#define FL_MAP_ONLY 1                           /* the map without the chip's RAM driver */
#include "fm1_flash.h"
#define PLAT_SMK37 1
#include "../include/plat.h"
#include "../template/app.h"                /* its APP_STORE_MAP on the SMK (PLAT_SMK37) */
static const uint32_t MAP[OBJ_NOBJ] = APP_STORE_MAP;

typedef struct { const char *name; uint32_t lo, hi; } win_t;
static const win_t PROT[] = {{"boot head", FL_PROT0_LO, FL_PROT0_HI}, {"BTIF + USRTRIM", FL_PROT1_LO, FL_PROT1_HI},
                             {"USR", FL_PROT2_LO, FL_PROT2_HI}, {"key_mac", FL_PROT3_LO, FL_PROT3_HI},
                             {"the app area", FL_APP_LO, FL_APP_HI}};
static const win_t WIN[] = {{"FL_XDATA0", FL_XDATA0_LO, FL_XDATA0_HI}, {"FL_AUTO", FL_AUTO_LO, FL_AUTO_HI},
                            {"FL_XDATA1", FL_XDATA1_LO, FL_XDATA1_HI}, {"FL_OTA", FL_OTA_LO, FL_OTA_HI},
                            {"FL_DATA", FL_DATA_LO, FL_DATA_HI}, {"FL_STOCK_MARK", FL_STOCK_MARK, FL_STOCK_MARK + 0x1000u},
                            {"FL_GLOB", FL_GLOB_LO, FL_GLOB_HI}};
#define N(a) (sizeof (a) / sizeof (a)[0])
static int bad;
static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    bad += !ok;
}
static int apart(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a1 <= b0 || b1 <= a0; }

int main(void)
{
    uint32_t i, j, o;
    char b[96];
    printf("SMK-37 flash map (1 MiB, JEDEC %#x): the app area %#x..%#x\n", (unsigned)FL_JEDEC_ID, (unsigned)FL_APP_LO,
           (unsigned)FL_APP_HI);
    for (i = 0; i < N(WIN); i++)
        printf("  %-14s %#07x..%#07x  %3u sectors\n", WIN[i].name, (unsigned)WIN[i].lo, (unsigned)WIN[i].hi,
               (unsigned)((WIN[i].hi - WIN[i].lo) >> 12));
    for (i = 0; i < N(WIN); i++) {
        const win_t *w = &WIN[i];
        int ok = !(w->lo & 0xFFFu) && !(w->hi & 0xFFFu) && w->lo < w->hi && w->hi <= FL_FLASH_SIZE && w->lo >= FL_APP_HI;
        snprintf(b, sizeof b, "%s: sector aligned, inside the part, above the app area", w->name);
        check(b, ok);
        for (j = 0; j < N(PROT); j++) {
            snprintf(b, sizeof b, "%s clear of %s", w->name, PROT[j].name);
            check(b, apart(w->lo, w->hi, PROT[j].lo, PROT[j].hi));
        }
        for (j = i + 1; j < N(WIN); j++) {
            snprintf(b, sizeof b, "%s clear of %s", w->name, WIN[j].name);
            check(b, apart(w->lo, w->hi, WIN[j].lo, WIN[j].hi));
        }
    }
    check("the update's staging is 5 sectors, its record at a 4 KiB boundary - 256 inside it",
          FL_OTA_HI - FL_OTA_LO == 0x5000u && FL_OTA_LO + 0x5000u - 0x100u > FL_OTA_LO);
    check("the autosave pair sits just under FL_XDATA1 (plat.h's PLAT_XDATA1 spans both)", FL_AUTO_HI == FL_XDATA1_LO);
    check("plat.h's windows are the board's", PLAT_DATA_LO == FL_DATA_LO && PLAT_DATA_HI == FL_DATA_HI &&
          PLAT_XDATA0_LO == FL_XDATA0_LO && PLAT_XDATA0_HI == FL_XDATA0_HI && PLAT_XDATA1_LO == FL_AUTO_LO &&
          PLAT_XDATA1_HI == FL_XDATA1_HI && PLAT_GLOB == FL_GLOB_LO && PLAT_WIN_HI == FL_DATA_HI);
    check("plat.h's fourth window (xdata2) is the board's settings pair, FL_GLOB",
          PLAT_XDATA2_LO == FL_GLOB_LO && PLAT_XDATA2_HI == FL_GLOB_HI);
    check("plat.h's update staging is the board's (backup.c's journal, the crash history, the debug stats in it)",
          PLAT_STAGE_LO == FL_OTA_LO && PLAT_STAGE_HI == FL_OTA_HI && PLAT_STAGE_LO >= PLAT_WIN_LO &&
          PLAT_STAGE_HI <= PLAT_WIN_HI);
    check("USRTRIM's record and its marker are inside the protected window, read only",
          FL_USRTRIM >= FL_PROT1_LO && FL_TRIM_MARK + 8u <= FL_PROT1_HI && !FL_RANGE_OK(FL_USRTRIM, 0x1000u) &&
          !FL_RANGE_OK(FL_BTIF, 0x1000u));
    check("nothing protected may be erased or written (FL_RANGE_OK)",
          !FL_RANGE_OK(0u, 0x1000u) && !FL_RANGE_OK(0x3000u, 0x1000u) && !FL_RANGE_OK(0xC0000u, 0x1000u) &&
          !FL_RANGE_OK(0xC1000u, 0x1000u) && !FL_RANGE_OK(0xF4000u, 0x1000u) && !FL_RANGE_OK(0xFD000u, 0x1000u) &&
          !FL_RANGE_OK(0xFF000u, 0x1000u) && !FL_RANGE_OK(0x4000u, 0x1000u) && !FL_RANGE_OK(0x9C000u, 0x1000u) &&
          !FL_RANGE_OK(0xBF000u, 0x2000u) && !FL_RANGE_OK(0xE9000u, 0x2000u));
    check("the stock marker sector may be erased (released once at the first boot), the store and the windows written",
          FL_RANGE_OK(FL_STOCK_MARK, 0x1000u) && FL_RANGE_OK(FL_DATA_LO, 0x1000u) && FL_RANGE_OK(FL_DATA_HI - 0x100u, 0x100u) &&
          FL_RANGE_OK(FL_GLOB_LO, 0x3000u) && FL_RANGE_OK(FL_XDATA0_LO, 0x4000u) && FL_RANGE_OK(FL_AUTO_LO, 0x2000u) &&
          FL_RANGE_OK(FL_OTA_LO, 0x5000u));
    check("update records are looked for above the app area and below USR only",
          FL_REC_LO == FL_APP_HI && FL_REC_HI == FL_PROT1_LO);
    /* the update loader's windows for this board (bsp/loader/loader.c LDR_SMK37): the app area, the records above it
     * up to BTIF and between the marker and USR; never the store nor a protected range */
    {
        uint32_t s, ok = 1;
        for (s = 0; s < FL_FLASH_SIZE; s += 0x1000u) {
            int ldr = s >= 0x4000u && s < 0xC0000u;
            int prot = (s < 0x4000u) || (s >= 0xC0000u && s < 0xC2000u) || (s >= 0xF4000u && s < 0xFE000u) || s >= 0xFF000u;
            int store = s >= FL_DATA_LO && s < FL_DATA_HI;
            if (ldr && (prot || store))
                ok = 0;
            if (s >= FL_APP_LO && s < FL_APP_HI && !ldr)
                ok = 0;
        }
        check("the loader's window covers the app area and no protected sector nor the store", (int)ok);
    }
    /* the template's objects on the SMK: a pair of sectors each inside the store or the settings pair, no overlaps */
    for (o = 0; o < OBJ_NOBJ; o++) {
        uint32_t a0 = MAP[o], a1 = a0 + 0x2000u;
        int ok = !(a0 & 0xFFFu) && (FL_IN(a0, 0x2000u, FL_DATA_LO, FL_DATA_HI) || FL_IN(a0, 0x2000u, FL_GLOB_LO, FL_GLOB_HI));
        for (j = 0; j < o; j++)
            if (!apart(a0, a1, MAP[j], MAP[j] + 0x2000u))
                ok = 0;
        snprintf(b, sizeof b, "the template's object %u at %#x: two sectors in the store, no overlap", (unsigned)o, (unsigned)a0);
        check(b, ok);
    }
    printf("%s\n", bad ? "SMK37 FLASH TEST FAILED" : "smk37 flash test passed");
    return bad != 0;
}
