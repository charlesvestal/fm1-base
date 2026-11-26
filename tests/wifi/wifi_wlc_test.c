/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David; in fm1-base: tests/wifi/run.sh */
/* The coexistence controller's packed table (device/fm1_wifi_wlc.h): every word comes back as it was recorded, both
 * unpacked here (FM1_WLC_WORD) and as felucca-bsp's real fm1_wifi_up (bsp/chip/fm1_wifi.h, its register writes
 * logged) writes it to the WLC port when device/wifi_own.c hands it the table. */
#include <stdio.h>
#include <stdint.h>
#include "fm1_wifi_wlc.h"
#include "wifi_wlc_ref.h"

#define LOG_MAX 2048u
static uint32_t port_words[LOG_MAX];
static unsigned port_n, port_over, other_wr;
static uint32_t sim_rd(uint32_t a)
{
    (void)a;
    return 0u;
}
static void sim_wr(uint32_t a, uint32_t v)
{
    if (a != 0x14004u) {
        other_wr++;
        return;
    }
    if (port_n < LOG_MAX)
        port_words[port_n++] = v;
    else
        port_over++;
}
#define FM1_WIFI_RD(a) sim_rd((uint32_t)(a))
#define FM1_WIFI_WR(a, v) sim_wr((uint32_t)(a), (uint32_t)(v))
#include "../../bsp/chip/fm1_wifi.h"   /* the real driver (not host/wifi_fake/fm1_wifi.h) */

int main(void)
{
    unsigned i, bad = 0, n = (unsigned)(sizeof wlc_ref / sizeof *wlc_ref);
    if (n != FM1_WLC_N) {
        printf("FAIL count %u vs %u\n", n, (unsigned)FM1_WLC_N);
        return 1;
    }
    for (i = 0; i < n; i++)
        if (FM1_WLC_WORD(i) != wlc_ref[i]) {
            bad++;
            printf("FAIL word %u: %08X, was %08X\n", i, (unsigned)FM1_WLC_WORD(i), (unsigned)wlc_ref[i]);
        }
    fm1_wifi_up(fm1_wlc_hi, fm1_wlc_c, FM1_WLC_N);
    if (port_n != n || port_over) {
        printf("FAIL fm1_wifi_up wrote %u words to the WLC port, want %u\n", port_n, n);
        bad++;
    }
    for (i = 0; i < n && i < port_n; i++)
        if (port_words[i] != wlc_ref[i]) {
            bad++;
            printf("FAIL fm1_wifi_up word %u: %08X, was %08X\n", i, (unsigned)port_words[i], (unsigned)wlc_ref[i]);
        }
    if (FM1_WLC_PORT != 0x14004u || other_wr == 0u) {
        printf("FAIL the port or the STA settings (%u other writes)\n", other_wr);
        bad++;
    }
    printf("wifi_wlc_test: %u words, through fm1_wifi_up %u, %u differ\n", n, port_n, bad);
    return (int)bad;
}
