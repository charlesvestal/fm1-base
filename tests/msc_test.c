/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Julius Vitkauskas */
/* Disk mode (device/msc.c) against a computer on the bulk-only transport, its USB side faked (usb_msc_*: what the
 * host sends, what it gets), over a drive of two media that can be pulled and swapped (plat_disk_t with read_fails,
 * sync and present): reads and writes; the residue counts data, not padding; no medium, and "medium changed" once a
 * medium is back or another one is in, even when no command saw it gone; a WRITE's blocks never reach a medium put in
 * during it, nor follow a failed block; a WRITE, SYNCHRONIZE CACHE and an eject answered by sync's result.
 *   cc -std=c99 -Iinclude -Idevice -o msc_test tests/msc_test.c && ./msc_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

/* the USB side, faked: the host's bytes in, ours out at once */
static uint8_t h_out[1 << 16], h_in[1 << 17];
static uint32_t h_out_r, h_out_w, h_in_n, t_ms;
static int usb_msc_ready(void) { return 1; }
static uint32_t usb_msc_avail(void) { return h_out_w - h_out_r; }
static uint32_t usb_msc_read(uint8_t *d, uint32_t max)
{
    uint32_t n = h_out_w - h_out_r;
    if (n > max)
        n = max;
    memcpy(d, h_out + h_out_r, n);
    h_out_r += n;
    return n;
}
static int usb_msc_busy(void) { return 0; }
static int usb_msc_send(const uint8_t *p, uint32_t n)
{
    memcpy(h_in + h_in_n, p, n);
    h_in_n += n;
    return 0;
}
static void usb_msc_pump(uint32_t out) { (void)out; t_ms++; }
static uint32_t usb_msc_resets(void) { return 0; }
static void usb_msc_switch(uint32_t on) { (void)on; }
uint32_t plat_ms(void) { return t_ms; }

#include "../device/msc.c"

/* the drive: two media, one in (or none) */
static uint8_t media[2][64][512];
static int cur, gone, sync_rc, fail_lba = -1, fail_read = -1;
static uint32_t writes[2], syncs, flushes;
static const int GEN[2] = {5, 9};
static int d_present(void) { return gone ? 0 : GEN[cur]; }
static const uint8_t *d_read(uint32_t lba) { return (int)lba == fail_read ? 0 : media[cur][lba]; }
static int d_write(uint32_t lba, const uint8_t *b)
{
    if ((int)lba == fail_lba)
        return -1;
    memcpy(media[cur][lba], b, 512);
    writes[cur]++;
    return 0;
}
static int d_sync(void) { syncs++; return sync_rc; }
static void d_flush(void) { flushes++; }
static plat_disk_t drive = {64, d_read, d_write, d_flush, "test", "drive", d_sync, 0, 1, d_present};

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { int status; uint32_t residue, data_n; uint8_t *data; } csw_t;

static void push(const void *p, uint32_t n)
{
    memcpy(h_out + h_out_w, p, n);
    h_out_w += n;
}

/* a command: its block, the OUT data pushed a block at a time (between(k) called before block k), polled until the
 * status comes */
static csw_t run(const uint8_t *cdb, uint32_t dlen, int in, const uint8_t *out, void (*between)(int k))
{
    static uint32_t tag = 100;
    uint8_t cbw[31] = {'U', 'S', 'B', 'C'};
    csw_t r = {-1, 0, 0, 0};
    uint32_t start = h_in_n, k, i;
    tag++;
    memcpy(cbw + 4, &tag, 4);
    memcpy(cbw + 8, &dlen, 4);
    cbw[12] = in ? 0x80 : 0;
    cbw[14] = 10;
    memcpy(cbw + 15, cdb, 10);
    push(cbw, 31);
    for (k = 0; !in && out && k * 512u < dlen; k++) {
        if (between)
            between((int)k);
        push(out + k * 512u, 512);
        for (i = 0; i < 4; i++)
            plat_disk_poll();
    }
    for (i = 0; i < 400 && h_in_n - start < 13u; i++)
        plat_disk_poll();
    for (i = 0; i < 400; i++) {                  /* until the status block (USBS, our tag) is the last thing in */
        uint8_t *c = h_in + h_in_n - 13;
        if (h_in_n - start >= 13u && !memcmp(c, "USBS", 4) && !memcmp(c + 4, &tag, 4))
            break;
        plat_disk_poll();
    }
    if (h_in_n - start >= 13u && !memcmp(h_in + h_in_n - 13, "USBS", 4)) {
        memcpy(&r.residue, h_in + h_in_n - 5, 4);
        r.status = h_in[h_in_n - 1];
        r.data_n = h_in_n - start - 13;
        r.data = h_in + start;
    }
    return r;
}

static void cdb10(uint8_t *c, uint8_t op, uint32_t lba, uint16_t n)
{
    memset(c, 0, 10);
    c[0] = op;
    c[2] = (uint8_t)(lba >> 24), c[3] = (uint8_t)(lba >> 16), c[4] = (uint8_t)(lba >> 8), c[5] = (uint8_t)lba;
    c[7] = (uint8_t)(n >> 8), c[8] = (uint8_t)n;
}

static void sense(uint8_t *key, uint8_t *asc)
{
    uint8_t c[10] = {0x03, 0, 0, 0, 18};
    csw_t r = run(c, 18, 1, 0, 0);
    *key = r.data ? r.data[2] : 0xFF;
    *asc = r.data ? r.data[12] : 0xFF;
}

static void swap_at_1(int k) { if (k == 1) cur = 1; }

int main(void)
{
    static uint8_t blocks[8 * 512];
    uint8_t c[10], key, asc;
    csw_t r;
    uint32_t i;
    for (i = 0; i < sizeof media; i++)
        ((uint8_t *)media)[i] = (uint8_t)(i * 7u + (i >> 9));
    plat_disk_mode(&drive);

    memset(c, 0, 10);                            /* TEST UNIT READY; READ two blocks */
    r = run(c, 0, 0, 0, 0);
    CHECK(r.status == 0);
    cdb10(c, 0x28, 3, 2);
    r = run(c, 1024, 1, 0, 0);
    CHECK(r.status == 0 && r.residue == 0 && r.data_n == 1024 && !memcmp(r.data, media[0][3], 1024));

    fail_read = 4;                               /* the 2nd block unreadable: an error, the residue its 512 */
    cdb10(c, 0x28, 3, 2);
    r = run(c, 1024, 1, 0, 0);
    sense(&key, &asc);
    CHECK(r.status == 1 && r.residue == 512 && key == 0x03 && asc == 0x11);
    fail_read = -1;

    gone = 1;                                    /* pulled: no medium, a READ's whole length not data */
    memset(c, 0, 10);
    r = run(c, 0, 0, 0, 0);
    sense(&key, &asc);
    CHECK(r.status == 1 && key == 0x02 && asc == 0x3A);
    cdb10(c, 0x28, 0, 1);
    r = run(c, 512, 1, 0, 0);
    CHECK(r.status == 1 && r.residue == 512 && r.data_n == 512);
    gone = 0;                                    /* back: "changed" once, then ready */
    memset(c, 0, 10);
    r = run(c, 0, 0, 0, 0);
    sense(&key, &asc);
    CHECK(r.status == 1 && key == 0x06 && asc == 0x28);
    r = run(c, 0, 0, 0, 0);
    CHECK(r.status == 0);

    cur = 1;                                     /* another medium in between two commands, never seen gone */
    r = run(c, 0, 0, 0, 0);
    sense(&key, &asc);
    CHECK(r.status == 1 && key == 0x06 && asc == 0x28);
    r = run(c, 0, 0, 0, 0);
    CHECK(r.status == 0);
    cur = 0;
    r = run(c, 0, 0, 0, 0);                      /* (and back: changed again) */
    CHECK(r.status == 1);

    for (i = 0; i < sizeof blocks; i++)          /* a WRITE of 4: written, then synced */
        blocks[i] = (uint8_t)(0xA0 ^ i);
    syncs = 0;
    cdb10(c, 0x2A, 10, 4);
    r = run(c, 2048, 0, blocks, 0);
    CHECK(r.status == 0 && r.residue == 0 && syncs == 1 && !memcmp(media[0][10], blocks, 2048));

    sync_rc = -1;                                /* its sync failing: the WRITE fails */
    r = run(c, 2048, 0, blocks, 0);
    sense(&key, &asc);
    CHECK(r.status == 1 && key == 0x03 && asc == 0x0C);
    sync_rc = 0;

    writes[0] = writes[1] = 0;                   /* another medium put in after the WRITE's 1st block: none of the
                                                  * rest reaches it */
    memcpy(blocks, media[1][20], 512);
    cdb10(c, 0x2A, 20, 4);
    r = run(c, 2048, 0, blocks, swap_at_1);
    CHECK(r.status == 1 && r.residue == 3 * 512 && writes[0] == 1 && writes[1] == 0);
    r = run((uint8_t[10]){0}, 0, 0, 0, 0);       /* (the next command hears it changed) */
    CHECK(r.status == 1);
    cur = 0;
    run((uint8_t[10]){0}, 0, 0, 0, 0);

    writes[0] = 0;                               /* the 2nd block failing: the 3rd and 4th never written */
    fail_lba = 31;
    cdb10(c, 0x2A, 30, 4);
    r = run(c, 2048, 0, blocks, 0);
    CHECK(r.status == 1 && r.residue == 3 * 512 && writes[0] == 1);
    fail_lba = -1;

    syncs = flushes = 0;                         /* SYNCHRONIZE CACHE: sync's answer */
    memset(c, 0, 10);
    c[0] = 0x35;
    r = run(c, 0, 0, 0, 0);
    CHECK(r.status == 0 && syncs == 1 && flushes == 0);
    sync_rc = -1;
    r = run(c, 0, 0, 0, 0);
    CHECK(r.status == 1);
    memset(c, 0, 10);                            /* an eject whose sync fails: refused, not ejected */
    c[0] = 0x1B;
    c[4] = 2;
    r = run(c, 0, 0, 0, 0);
    CHECK(r.status == 1 && !ms.ejected);
    sync_rc = 0;
    r = run(c, 0, 0, 0, 0);
    CHECK(r.status == 0 && ms.ejected);

    if (fails) {
        printf("msc: %d FAILED\n", fails);
        return 1;
    }
    printf("msc: ok\n");
    return 0;
}
