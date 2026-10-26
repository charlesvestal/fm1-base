/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
/* Macro definitions come from the real BSP through its C preprocessor,
 * with the open build's settings (open_ble/hal/fm1_ble_flash.h). */
#include "flash_ranges.inc"
/* The BSP's optional flash-window hooks, as the open build selects them. */
_Static_assert(FL_IRQ_MASK_LEVEL==6,"flash window: ICFG gate, nesting");
_Static_assert(FL_ERR_REFUSED==-8 && FL_ERR_LATE_READY==-9,"open flash result codes");
#ifndef FL_CAN_START
#error "open build: no erase/program without the idle gate"
#endif
int main(void)
{
    assert(!FL_RANGE_OK(0x93000u,4096u));
    assert(!FL_RANGE_OK(0xe9000u,4096u));
    assert(!FL_RANGE_OK(0x92fffu,2u));
    assert(!FL_RANGE_OK(0xe8fffu,2u));
    assert(FL_RANGE_OK(0x94000u,4096u) && FL_RANGE_OK(0x95000u,4096u) && FL_RANGE_OK(0x96000u,4096u));
    assert(FL_RANGE_OK(0xde000u,8192u));
    assert(FL_RANGE_OK(0xfc000u,8192u));
    assert(FL_RANGE_OK(0xe0000u,4096u));
    assert(!FL_RANGE_OK(0xffffffffu,2u));
    assert(!FL_RANGE_OK(0x97000u,0xffff0000u));
    puts("actual BSP flash ranges: calibration/BTIF protected, former seed/journal XDATA free, A/B store allowed, overflow refused; idle gate, ICFG window, -8/-9 codes selected: PASS");
    return 0;
}
