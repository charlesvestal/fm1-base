/* SPDX-License-Identifier: MPL-2.0 */
/* This FM-1's four hex digits (plat.h plat_dev_id): the hash of the flash's unique ID, as bt_own.c own_addr and radio.c
 * ble_midi_name make it (FNV-1a over the 16 octets, then h ^= h >> 16): the low two octets of the Bluetooth address
 * made for it, so "FM1-XXXX" is the same four digits on the radio, on USB and in the tools. -1: no ID (all equal octets) */
#ifndef PLAT_UID_H
#define PLAT_UID_H
#include <stdint.h>

static inline int plat_uid_hash16(const uint8_t *u)
{
    uint32_t h = 2166136261u, i = 1;
    while (u && i < 16u && u[i] == u[0])          /* all FF or all 0: no ID */
        i++;
    if (!u || i >= 16u)
        return -1;
    for (i = 0; i < 16u; i++)
        h = (h ^ u[i]) * 16777619u;
    h ^= h >> 16;
    return (int)(h & 0xFFFFu);
}

/* "FM1-" and the four digits (upper case) into out[9]; id < 0: "FM1-0000" (no ID: the digits of nothing) */
static inline void plat_dev_name(char out[9], int id)
{
    static const char HEX[] = "0123456789ABCDEF";
    uint32_t i;
    out[0] = 'F', out[1] = 'M', out[2] = '1', out[3] = '-';
    for (i = 0; i < 4u; i++)
        out[4 + i] = id < 0 ? '0' : HEX[((uint32_t)id >> (12u - 4u * i)) & 15u];
    out[8] = 0;
}
#endif
