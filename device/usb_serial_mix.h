// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Henri David
#ifndef USB_SERIAL_MIX_H
#define USB_SERIAL_MIX_H
/* The USB serial number (fm1.toml usb_serial; device/plat_fm1.c fm1_usb_serial): the chip's 16-byte ID folded into 12 bytes
 * (the last 4 over the first 4), as 24 hex digits.
 *
 * With a digest (FM1B_USB_SERIAL_MIX, 24 hex digits that tools/build.py makes from the USB audio configuration and the
 * firmware's id_prefix, only when one of those options is not at its default) its 12 bytes are XORed into the folded ID:
 * a firmware with another configuration has another serial on the same chip, so a computer that binds a device to its
 * serial (Windows) sees a new device when the descriptors change, and two chips still have two serials (the XOR with
 * one digest is one-to-one). No digest: the serial as before, byte for byte. No chip ID: "" without a digest (the bsp
 * then says "0"), the digest alone with one. */
#include <stdint.h>

static inline uint8_t usb_serial_nibble(char c)
{
    return (uint8_t)(c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0);
}

/* sn: 25 bytes, the result's; u: the chip's 16-byte ID or 0; mix: the digest's 24 hex digits or 0. Without a digest, the
 * loop as it was before it (the firmwares out there: the same code as well as the same serial) */
static inline const char *usb_serial_make(char sn[25], const uint8_t *u, const char *mix)
{
    uint32_t i;
    if (!mix) {
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
    for (i = 0; i < 12u; i++) {
        uint8_t b = u ? (uint8_t)(u[i] ^ (i < 4u ? u[12u + i] : 0u)) : 0u;
        b ^= (uint8_t)((usb_serial_nibble(mix[2u * i]) << 4) | usb_serial_nibble(mix[2u * i + 1u]));
        sn[2u * i] = "0123456789ABCDEF"[b >> 4];
        sn[2u * i + 1u] = "0123456789ABCDEF"[b & 15u];
    }
    sn[24] = 0;
    return sn;
}
#endif
