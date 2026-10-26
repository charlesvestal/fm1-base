/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FELUCCA_BLE_MEMORY_H
#define FELUCCA_BLE_MEMORY_H
#include <stddef.h>
#ifdef BLE_HOST_TEST
#include <string.h>
#else
/* Felucca supplies these; the pi32v2 toolchain has no libc headers/library. */
void *memcpy(void *dst,const void *src,unsigned n);
void *memset(void *dst,int value,unsigned n);
int memcmp(const void *a,const void *b,unsigned n);
#endif
#endif
