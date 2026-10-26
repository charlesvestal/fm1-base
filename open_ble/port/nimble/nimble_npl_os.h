/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FELUCCA_NPL_OS_H
#define FELUCCA_NPL_OS_H
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t ble_npl_time_t;
typedef int32_t ble_npl_stime_t;
#ifdef BLE_HOST_TEST
#define BLE_NPL_OS_ALIGNMENT __SIZEOF_POINTER__
#else
#define BLE_NPL_OS_ALIGNMENT 4u
#endif
#define BLE_NPL_TIME_FOREVER UINT32_MAX
struct ble_npl_event { struct ble_npl_event *next; void (*fn)(struct ble_npl_event *); void *arg; bool queued; };
struct ble_npl_eventq { struct ble_npl_event *head,*tail; };
struct ble_npl_mutex { unsigned depth; };
struct ble_npl_sem { uint16_t tokens; };
struct ble_npl_callout {
    struct ble_npl_event ev; struct ble_npl_eventq *q;
    struct ble_npl_callout *next; uint32_t deadline; bool active;
};
#endif
