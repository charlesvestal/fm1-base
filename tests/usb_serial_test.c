// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Henri David
/* The USB serial number (device/usb_serial_mix.h, fm1_usb_serial in device/plat_fm1.c): with no digest (every USB audio
 * option off) exactly the serial of the firmwares out there, for any chip ID; with a digest another serial, the same
 * for the same digest, different for two, still 24 hex digits, still one per chip. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "usb_serial_mix.h"

static int failed;
#define OK(c, ...) do { int ok_ = !!(c); printf("  %s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); failed += !ok_; } while (0)

/* the serial as it was before the digest (device/plat_fm1.c at fm1-base main), copied here word for word */
static const char *old_serial(char sn[25], const uint8_t *u)
{
    uint32_t i;
    if (!u)
        return "";
    for (i = 0; i < 12u; i++) {
        uint8_t b = (uint8_t)(u[i] ^ (i < 4u ? u[12u + i] : 0u));
        sn[2u * i] = "0123456789ABCDEF"[b >> 4];
        sn[2u * i + 1u] = "0123456789ABCDEF"[b & 15u];
    }
    sn[24] = 0;
    return sn;
}

static int hex24(const char *s)
{
    int i;
    if (strlen(s) != 24)
        return 0;
    for (i = 0; i < 24; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F')))
            return 0;
    return 1;
}

int main(void)
{
    static const uint8_t uid[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98, 0x10, 0x20, 0x30, 0x40};
    static const uint8_t uid2[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98, 0x10, 0x20, 0x30, 0x41};
    static const char mixA[] = "A9D5F67D730C2C3F57922BCA", mixB[] = "5A5A5A5A0123456789ABCDEF";
    char s0[25], s1[25], s2[25], s3[25], s4[25], s5[25];
    const char *d, *a, *b;
    uint8_t r[16];
    uint32_t seed = 12345, k;
    int i, same = 1;

    /* the default: a fixed chip ID gives the literal old string, and the old function's, with no digest */
    d = usb_serial_make(s0, uid, 0);
    OK(!strcmp(d, "1103752789ABCDEFFEDCBA98"), "no digest: the literal old serial for a fixed chip ID (%s)", d);
    OK(!strcmp(d, old_serial(s1, uid)), "no digest: the old function's serial for it");
    OK(!strcmp(usb_serial_make(s2, 0, 0), ""), "no digest, no chip ID: empty, as before (the bsp then says 0)");
    for (k = 0; k < 2000; k++) {                      /* any chip ID: the old serial, byte for byte */
        for (i = 0; i < 16; i++) {
            seed = seed * 1664525u + 1013904223u;
            r[i] = (uint8_t)(seed >> 24);
        }
        if (strcmp(usb_serial_make(s3, r, 0), old_serial(s4, r)))
            same = 0;
    }
    OK(same, "no digest: the old serial for 2000 other chip IDs");

    /* with a digest */
    a = usb_serial_make(s3, uid, mixA);
    b = usb_serial_make(s4, uid, mixB);
    OK(hex24(a) && hex24(b), "a digest: 24 hex digits (%s, %s)", a, b);
    OK(strcmp(a, d) != 0, "a digest: not the old serial");
    OK(strcmp(a, b) != 0, "two digests: two serials on one chip");
    OK(!strcmp(a, usb_serial_make(s5, uid, mixA)), "the same digest: the same serial");
    OK(strcmp(usb_serial_make(s5, uid2, mixA), a) != 0, "a digest: two chips keep two serials");
    OK(!strcmp(usb_serial_make(s5, uid, "000000000000000000000000"), d), "a digest of zeros: the old serial (nothing mixed)");
    OK(hex24(usb_serial_make(s5, 0, mixA)) && strcmp(s5, usb_serial_make(s2, 0, mixB)) != 0,
       "a digest and no chip ID: the digest alone, 24 hex digits, still one per configuration");
    OK(!strcmp(usb_serial_make(s5, uid, "a9d5f67d730c2c3f57922bca"), a), "the digest in lower case is the same");

    printf("usb-serial-c: %s\n", failed ? "FAILED" : "all passed");
    return failed != 0;
}
