/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FELUCCA_LIBC_ASSERT_H
#define FELUCCA_LIBC_ASSERT_H
void ble_host_assert(const char *,int);
#define assert(x) ((x) ? (void)0 : ble_host_assert(__FILE__,__LINE__))
#endif
