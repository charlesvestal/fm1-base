/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
/* The open BLE build's settings for felucca-bsp's flash driver (bsp/chip/fm1_flash.h), and the plain-window store
 * read. device/unity.c includes this, in open builds only, in place of fm1_flash.h; the update loader never sees it.
 *
 * - FL_RANGE_OK: the BSP's app window minus the two calibration sectors the controller reads at start (r8 04 §7):
 *   the first VM sector 0x93000..0x93FFF (inside XDATA0) and BTIF 0xE9000..0xE9FFF (inside XDATA1). The RAM driver
 *   refuses an erase or program there (-1), whoever asks (store, plat_data_*, OTA staging).
 * - FL_IRQ_MASK_LEVEL 6: a flash window clears ICFG bit 8, as the controller's own nesting locks do, and gives the
 *   saved ICFG back (nests inside them). Priority 7 holds only the CPU exception; nothing of BLE runs there.
 * - FL_CAN_START: no erase or page program starts unless the radio is idle and no link is up (ble_port_storage_idle,
 *   device/ble_open_platform.c), decided with interrupts held off, so a CONNECT_IND cannot slip in between the check
 *   and the command. A refusal returns -8 (FL_ERR_REFUSED).
 * - FL_ERR_LATE_READY -9: a part that finished after the normal bound (open_ble/ble_store.c: BLE_FLASH_LATE_READY).
 * fl_erase4k / fl_write: never back to XIP while the part is busy, reset after the extended bound (the BSP's own). */
#pragma once
#ifdef FL_XIP
#error "fm1_flash.h was included before the open build's settings"
#endif
#define FL_CAL_CLEAR(off,n) (((uint32_t)(off)+(uint32_t)(n)<=0x93000u || (uint32_t)(off)>=0x94000u) && \
                            ((uint32_t)(off)+(uint32_t)(n)<=0xE9000u || (uint32_t)(off)>=0xEA000u))
#define FL_RANGE_OK(off,n) ((FL_STORE_OK(off,n) || FL_XDATA_OK(off,n) || FL_IN(off,n,FL_OTA_LO,FL_OTA_HI)) && FL_CAL_CLEAR(off,n))
#define FL_IRQ_MASK_LEVEL 6
#define FL_ERR_LATE_READY (-9)
static int fm1_ble_flash_allowed(void);
#define FL_CAN_START() fm1_ble_flash_allowed()
#include "fm1_flash.h"

/* The store's reads (st_read) in open builds: through the unencrypted XIP window that fl_plain_window_init maps from
 * 0x93000 up, so a read never switches the flash off or holds interrupts off, also during a connection (the BSP's
 * fl_read_ram does both, in 256-byte windows). Main context only; erases and programs run inside their own window. */
static int fl_read_plain(uint32_t off,void *dst,uint32_t n)
{
    if(off<0x93000u || off>0x100000u || n>0x100000u-off) return -1;
    uint8_t *p=dst; const volatile uint8_t *s=(const volatile uint8_t *)(uintptr_t)FL_XIP(off);
    while(n--) *p++=*s++;
    return 0;
}
