/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Julius Vitkauskas */
/* fm1-base: what FatFs (ff.c) takes from <string.h>, on the device, which has no C library headers: memset, memcpy
 * and memcmp are felucca-bsp's chip/libc.c (in device/unity.c's unit before this); strchr is here (as the C library's:
 * the terminator found too) */
#pragma once
static const char *ff_strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return s;
        if (!*s)
            return 0;
    }
}
#define strchr(s, c) ff_strchr(s, c)
