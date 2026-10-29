/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David */
/* The default password of an Optimist access point (wifi_ap.h wifi_ap_default_key): in wifi_ap.c with MODE AP, and on its own
 * in a station-only build, whose MODE JAM joins such an access point with it (device/wifi_own.c). Included once. */
#ifndef WIFI_AP_KEY_C
#define WIFI_AP_KEY_C
#include "wifi_ap.h"

/* The default password of an Optimist access point: a function of its name alone, so another Optimist that hears the name
 * knows the key (Link by itself). Two FNV-1a (32-bit, offset 0x811C9DC5, prime 0x01000193) runs over a fixed salt and the
 * name's bytes, the salts "OPTIMIST-AP-KEY/1:" and "OPTIMIST-AP-KEY/2:"; each character is five bits of a hash, from the top
 * (A: bits 31..7 give characters 0..5, B: 31..22 characters 6..7), looked up in a 32-place table of 23 letters (no i, l, o)
 * and 8 digits (no 0, 1), whose last place repeats 'w'. 8 characters, so a WPA2 passphrase. This is not secret: anyone who
 * hears the name can compute it; it keeps strangers' phones off by accident only. Version 1: never changed (a change would
 * lock every FM-1 already joined out). */
static uint32_t ap_key_fnv(const char *salt, const uint8_t *ssid, uint32_t len)
{
    uint32_t h = 0x811C9DC5u, i;
    for (i = 0; salt[i]; i++)
        h = (h ^ (uint8_t)salt[i]) * 0x01000193u;
    for (i = 0; i < len; i++)
        h = (h ^ ssid[i]) * 0x01000193u;
    return h;
}
void wifi_ap_default_key(const uint8_t *ssid, uint32_t len, char out[WIFI_AP_KEY_LEN + 1u])
{
    static const char T[33] = "abcdefghjkmnpqrstuvwxyz23456789w";
    uint32_t a = ap_key_fnv("OPTIMIST-AP-KEY/1:", ssid, len), b = ap_key_fnv("OPTIMIST-AP-KEY/2:", ssid, len), i;
    for (i = 0; i < 6u; i++)
        out[i] = T[(a >> (27u - 5u * i)) & 31u];
    for (i = 0; i < 2u; i++)
        out[6u + i] = T[(b >> (27u - 5u * i)) & 31u];
    out[WIFI_AP_KEY_LEN] = 0;
}
#endif
