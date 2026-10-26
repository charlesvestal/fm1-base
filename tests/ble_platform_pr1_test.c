/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define FM1B_CPU2 1
#include "../include/plat.h"
static uint32_t fm1_ms,feeds,polls,jobs,job_arg;
static void fm1_wdt_feed(void) { feeds++; }
static void ble_poll(void) { polls++; }
#include "usb_wait_seam.inc"
#include "cpu2_host_seam.inc"
#define FM1_TICKS_PER_US 24u
static uint32_t icfg=0x180u,ticks;
static uint32_t irq_save(void) { uint32_t f=icfg; icfg&=~0x100u; return f; }
static void irq_restore(uint32_t f) { icfg=f; }
static uint32_t fm1_ticks(void) { return ticks; }
#include "lock_seam.inc"
static void job(uint32_t arg) { jobs++; job_arg=arg; }
int main(void)
{
    uint32_t outer=plat_lock(); assert(icfg==0x80u);
    uint32_t inner=plat_lock(); ticks+=240u;
    plat_unlock(inner); assert(icfg==0x80u);
    plat_unlock(outer); assert(icfg==0x180u && lock_max==10u);
    icfg=0x80u; outer=plat_lock(); plat_unlock(outer); assert(icfg==0x80u);
    (void)in_tick;
    fm1_ms=1; ble_usb_host_wait(); assert(feeds==1 && polls==1);
    for(unsigned i=0;i<100;i++) ble_usb_host_wait();
    assert(feeds==101 && polls==1);
    fm1_ms=2; ble_usb_host_wait(); assert(polls==2);
    fm1_ms=0xffffffffu; ble_usb_host_wait();
    fm1_ms=0; ble_usb_host_wait(); assert(polls==4);
    assert(!plat_cpu2_on() && !plat_cpu2_run(job,1) && !jobs);
    assert(!plat_cpu2_set(1) && plat_cpu2_on());
    assert(plat_cpu2_run(job,0x1234u) && jobs==1 && job_arg==0x1234u);
    plat_cpu2_sync(); assert(!plat_cpu2_busy() && !plat_cpu2_wait());
    assert(!plat_cpu2_bad() && !plat_cpu2_gave_up() && !plat_cpu2_barred());
    assert(!plat_cpu2_set(0) && !plat_cpu2_run(job,2) && jobs==1);
    puts("actual USB host wait adapter (coalescing/wrap) and upstream CPU2 host worker (on/job/wait/off): PASS");
    return 0;
}
