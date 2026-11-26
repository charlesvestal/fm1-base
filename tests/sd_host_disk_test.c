/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Julius Vitkauskas */
/* The SD card as a USB drive on the host simulator's terms: lib/sd_fs.c with SD_NO_STREAMS (host/plat_host.c: the
 * streams as plain sector calls) over a card in memory. A WRITE's blocks reach the card by sync, and read back; reads
 * run ahead. (The device's streams are tests/sd_test.c's.)
 *   cc -std=c99 -DPLAT_HOST -Iinclude -Ilib -o sd_host_disk_test tests/sd_host_disk_test.c && ./sd_host_disk_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#define CARD 4096u
static uint8_t card[CARD][512];
static uint32_t now, reads, writes;
uint32_t plat_ms(void) { return now++; }
static int st = 1;
int plat_sd_state(void) { return st; }
uint32_t plat_sd_sectors(void) { return st == 1 ? CARD : 0; }
int plat_sd_read(uint32_t lba, void *buf, uint32_t n)
{
    if (lba + n > CARD)
        return SD_E_RANGE;
    memcpy(buf, card[lba], n * 512u);
    reads++;
    return 0;
}
int plat_sd_write(uint32_t lba, const void *buf, uint32_t n)
{
    if (lba + n > CARD)
        return SD_E_RANGE;
    memcpy(card[lba], buf, n * 512u);
    writes += n;
    return 0;
}
void plat_sd_info(plat_sd_info_t *i) { memset(i, 0, sizeof *i); }
static int sd_card_start(void) { st = 1; return 0; }
static void sd_card_stop(void) { st = 0; }
static int sd_card_check(void) { return st == 1; }
int plat_disk_poll(void) { return PLAT_DISK_MOUNTED; }
uint32_t plat_disk_idle_ms(void) { return 0; }

#define FM1B_MSC 1
#define SD_NO_STREAMS
#include "../lib/sd_fs.c"

int main(void)
{
    static uint8_t b[512];
    const plat_disk_t *d;
    uint32_t i, k, bad = 0;
    int fails = 0;
    for (i = 0; i < CARD; i++)
        for (k = 0; k < 512; k++)
            card[i][k] = (uint8_t)(i + k);
    d = plat_sd_disk();
    if (!d) {
        printf("sd-host-disk: no drive\n");
        return 1;
    }
    for (i = 100; i < 200; i++) {                /* a WRITE of 100 blocks, as disk mode gives it */
        for (k = 0; k < 512; k++)
            b[k] = (uint8_t)(i * 3 + k);
        if (d->write(i, b))
            fails++;
        d->work();
    }
    if (d->sync())
        fails++;
    for (i = 100; i < 200; i++)
        for (k = 0; k < 512; k++)
            bad += card[i][k] != (uint8_t)(i * 3 + k);
    if (bad || writes != 100)
        fails++;
    for (i = 300, bad = 0; i < 700; i++) {       /* reads, ahead */
        const uint8_t *r = d->read(i);
        if (!r || memcmp(r, card[i], 512))
            bad++;
        d->work();
    }
    if (bad)
        fails++;
    printf("sd-host-disk: %u blocks written, %u reads%s\n", writes, reads, fails ? ", FAILED" : "");
    return fails ? 1 : 0;
}
