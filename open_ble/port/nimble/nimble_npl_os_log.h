/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#ifndef FELUCCA_NPL_LOG_H
#define FELUCCA_NPL_LOG_H
#undef BLE_NPL_LOG
#define BLE_NPL_LOG(lvl, ...) do { } while(0)
static inline void ble_log_noop(const char *fmt,...) { (void)fmt; }
#undef BLE_NPL_LOG
#define BLE_NPL_LOG(lvl,...) do { if(0) ble_log_noop(__VA_ARGS__); } while(0)
#define BLE_HS_LOG_ERROR(...) BLE_NPL_LOG(ERROR,__VA_ARGS__)
#define BLE_HS_LOG_DEBUG(...) BLE_NPL_LOG(DEBUG,__VA_ARGS__)
#define BLE_EATT_LOG_ERROR(...) BLE_NPL_LOG(ERROR,__VA_ARGS__)
#define BLE_EATT_LOG_DEBUG(...) BLE_NPL_LOG(DEBUG,__VA_ARGS__)
#endif
