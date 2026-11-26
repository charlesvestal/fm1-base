/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Henri David */
/* lib/fatfs (FatFs R0.16, lib/sd_fs.c's) on a volume in memory: what fm1-base changed in it.
 *   expand   f_expand's search for a contiguous run wraps round the volume's end to cluster 2: the free clusters at
 *            the end and those from 2 are two runs, not one (R0.16 counted them as one: FR_INT_ERR, or a chain
 *            past the FAT's end)
 *   unlink   f_unlink_entry: a rename cut by a power loss (f_rename writes the new entry, then removes the old one)
 *            leaves one cluster chain under two names; f_unlink of either frees the chain the other still names,
 *            f_unlink_entry removes the name alone
 *   cc -std=c99 -DFF_USE_MKFS=1 -Ilib -o fatfs_test tests/fatfs_test.c && ./fatfs_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fatfs/ff.h"
#include "fatfs/diskio.h"

#define SECTORS 4096u                          /* 2 MiB: FAT12, a sector a cluster */
static uint8_t disk[SECTORS][512];
static uint8_t *snap[16];                      /* the volume after each write of a step (cut tests) */
static int snaps, snapping;

DSTATUS disk_status(BYTE pdrv) { return pdrv ? STA_NOINIT : 0; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }
DRESULT disk_read(BYTE pdrv, BYTE *buf, LBA_t sector, UINT n)
{
    if (pdrv || sector + n > SECTORS)
        return RES_PARERR;
    memcpy(buf, disk[sector], n * 512u);
    return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT n)
{
    if (pdrv || sector + n > SECTORS)
        return RES_PARERR;
    memcpy(disk[sector], buf, n * 512u);
    if (snapping && snaps < 16) {
        snap[snaps] = malloc(sizeof disk);
        memcpy(snap[snaps++], disk, sizeof disk);
    }
    return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buf)
{
    if (pdrv)
        return RES_PARERR;
    if (cmd == GET_SECTOR_COUNT)
        *(LBA_t *)buf = SECTORS;
    else if (cmd == GET_SECTOR_SIZE)
        *(WORD *)buf = 512;
    else if (cmd == GET_BLOCK_SIZE)
        *(DWORD *)buf = 1;
    return RES_OK;
}

#include "fatfs/ff.c"
#include "fatfs/ffunicode.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static FATFS fs;

static void fresh(void)                        /* a blank FAT16 volume, mounted */
{
    static uint8_t work[4096];
    MKFS_PARM opt = {FM_FAT, 0, 0, 0, 0};
    memset(disk, 0, sizeof disk);
    CHECK(f_mkfs("", &opt, work, sizeof work) == FR_OK);
    CHECK(f_mount(&fs, "", 1) == FR_OK);
}

static DWORD nfree(void)
{
    DWORD n = 0;
    FATFS *p;
    CHECK(f_getfree("", &n, &p) == FR_OK);
    return n;
}

static void expand(void)
{
    FIL f;
    FRESULT r;
    uint32_t cl, n;
    printf("fatfs: f_expand round the volume's end\n");
    fresh();
    cl = (uint32_t)fs.csize * 512u;
    n = fs.n_fatent - 2u;
    CHECK(nfree() == n);
    fs.last_clst = 2;
    CHECK(f_open(&f, "X.BIN", FA_CREATE_NEW | FA_WRITE) == FR_OK && f_expand(&f, 3u * cl, 1) == FR_OK);
    CHECK(f.obj.sclust == 2u && f_close(&f) == FR_OK);
    CHECK(f_open(&f, "A.BIN", FA_CREATE_NEW | FA_WRITE) == FR_OK && f_expand(&f, (n - 9u) * cl, 1) == FR_OK);
    CHECK(f_close(&f) == FR_OK);
    CHECK(f_unlink("X.BIN") == FR_OK);         /* 3 free at the start, 6 at the end */
    fs.last_clst = fs.n_fatent - 8u;           /* the search from A's last cluster */
    CHECK(f_open(&f, "C.BIN", FA_CREATE_NEW | FA_WRITE) == FR_OK);
    r = f_expand(&f, 8u * cl, 1);
    printf("  8 clusters asked, 6 free at the end and 3 at the start: %d\n", r);
    CHECK(r == FR_DENIED);                     /* (R0.16: FR_INT_ERR, a chain past the FAT's end) */
    CHECK(f_expand(&f, 6u * cl, 1) == FR_OK && f.obj.sclust == fs.n_fatent - 6u && f_close(&f) == FR_OK);
    CHECK(f_open(&f, "D.BIN", FA_CREATE_NEW | FA_WRITE) == FR_OK && f_expand(&f, 3u * cl, 1) == FR_OK);
    CHECK(f.obj.sclust == 2u && f_close(&f) == FR_OK);   /* the 3 at the start: a run after the wrap */
    CHECK(nfree() == 0);
    CHECK(f_mount(0, "", 0) == FR_OK);
}

static int has(const char *name, const uint8_t *data, UINT size)
{
    static uint8_t b[8192];
    FIL f;
    UINT got = 0;
    int ok = f_open(&f, name, FA_READ) == FR_OK && f_read(&f, b, sizeof b, &got) == FR_OK && got == size &&
             !memcmp(b, data, size);
    f_close(&f);
    return ok;
}

static void unlink_entry(void)
{
    static uint8_t data[5000];
    FIL f;
    FILINFO fi;
    UINT bw = 0;
    DWORD before;
    int k, cut = -1;
    printf("fatfs: a rename cut by a power loss, and f_unlink_entry\n");
    for (k = 0; k < (int)sizeof data; k++)
        data[k] = (uint8_t)(k * 7 + 1);
    fresh();
    CHECK(f_mkdir("SUB") == FR_OK);            /* (the new name in another folder's sector: two writes, a cut between) */
    CHECK(f_open(&f, "SONG.TMP", FA_CREATE_NEW | FA_WRITE) == FR_OK && f_write(&f, data, sizeof data, &bw) == FR_OK);
    CHECK(bw == sizeof data && f_close(&f) == FR_OK);
    snapping = 1;
    CHECK(f_rename("SONG.TMP", "SUB/SONG.BIN") == FR_OK);
    snapping = 0;
    for (k = 0; k < snaps && cut < 0; k++) {   /* the cut: the new name written, the old one not yet removed */
        memcpy(disk, snap[k], sizeof disk);
        CHECK(f_mount(&fs, "", 1) == FR_OK);
        if (f_stat("SONG.TMP", &fi) == FR_OK && f_stat("SUB/SONG.BIN", &fi) == FR_OK)
            cut = k;
    }
    printf("  %d writes in the rename; both names after write %d\n", snaps, cut + 1);
    CHECK(cut >= 0);
    CHECK(has("SONG.TMP", data, sizeof data) && has("SUB/SONG.BIN", data, sizeof data));
    before = nfree();
    CHECK(f_unlink_entry("SONG.TMP") == FR_OK);
    CHECK(f_stat("SONG.TMP", &fi) == FR_NO_FILE && has("SUB/SONG.BIN", data, sizeof data));
    CHECK(nfree() == before);                  /* the chain kept: SONG.BIN's */
    CHECK(f_mount(0, "", 0) == FR_OK && f_mount(&fs, "", 1) == FR_OK && has("SUB/SONG.BIN", data, sizeof data));
    CHECK(f_mkdir("DIR") == FR_OK && f_unlink_entry("DIR") == FR_DENIED);   /* files only */
    CHECK(f_unlink_entry("NONE.BIN") == FR_NO_FILE);

    memcpy(disk, snap[cut], sizeof disk);      /* what f_unlink would have done: the chain freed under the other */
    CHECK(f_mount(&fs, "", 1) == FR_OK);
    before = nfree();
    CHECK(f_unlink("SONG.TMP") == FR_OK && nfree() > before);
    for (k = 0; k < snaps; k++)
        free(snap[k]);
    snaps = 0;
    CHECK(f_mount(0, "", 0) == FR_OK);
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    expand();
    unlink_entry();
    if (fails) {
        printf("fatfs: %d FAILED\n", fails);
        return 1;
    }
    printf("fatfs: ok\n");
    return 0;
}
