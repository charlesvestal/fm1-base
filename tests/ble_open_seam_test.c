/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../include/plat.h"
#include "../open_ble/ble_host.h"
static uint32_t now,crashes,start_calls,off_calls,on_calls;
static uint8_t ble_started,start_fail,fault,link,subscribed,sticky,off_reason;
static struct { uint32_t magic,count; } fm1_crash;
static struct { uint8_t p3_rst; } fm1_boot;
#define FM1_CRASH_MAGIC 0x43525348u
static ble_host_diag_t host;
uint32_t ble_port_lock(void) { return 0; }
void ble_port_unlock(uint32_t k) { (void)k; }
static int ble_platform_start(void)
{
    start_calls++; if(start_fail) return -1;
    ble_started=host.running=1; return 0;
}
int ble_port_storage_idle(void) { return !link; }
int ble_port_failed(void) { return fault; }
void ble_host_enable(int on)
{
    if(on) on_calls++; else { off_calls++; if(!sticky) link=subscribed=0; }
}
void ble_host_off_reason(uint8_t reason) { off_reason=reason; }
int ble_host_pause_advertising(void) { return link ? -8 : 0; }
void ble_host_resume_advertising(void) {}
void ble_host_worker(void) { now++; host.synced=1; }
void ble_host_get_diag(ble_host_diag_t *d) { *d=host; }
int ble_host_linked(void) { return link; }
int ble_host_connected(void) { return link && subscribed; }
uint32_t ble_host_midi_room(void) { return subscribed ? 19 : 0; }
int ble_host_midi_take(uint32_t *pkt,uint32_t *ms) { *pkt=0x643c9009; *ms=now; return subscribed; }
void ble_host_midi_out(uint32_t pkt,uint32_t ms) { assert(pkt==0x643c9009); (void)ms; }
static uint32_t fm1_ms;
static uint32_t fm1_ticks(void) { return now*24000u; }
static void fm1_wdt_feed(void) { fm1_ms++; }
#include "../device/ble_open.c"
static void reboot(int cold)
{
    fm1_boot.p3_rst=cold ? 1 : 0;
    fm1_crash.magic=FM1_CRASH_MAGIC; fm1_crash.count=crashes;
    ble_started=ble_state=ble_applied=ble_wanted=0; memset(&host,0,sizeof host);
    radio_boot_note();
}
int main(void)
{
    uint32_t pkt,from;
    reboot(1); assert(!radio_last_bad && plat_ble_state()==BLE_OFF);
    plat_ble_start(); assert(plat_ble_state()==BLE_STARTING); ble_poll();
    assert(plat_ble_state()==BLE_READY && start_calls==1 && open_log.active);
    assert(!ble_open_store_begin()); ble_open_store_end();
    link=1; assert(ble_open_store_begin()==-8); assert(plat_ble_linked() && !plat_ble_connected() && !ble_midi_out_room(TO_BLE));
    subscribed=1; assert(plat_ble_connected() && ble_midi_out_room(TO_BLE)==19);
    assert(ble_midi_in(&pkt,&from) && pkt==0x643c9009); assert(from==TO_BLE_HOST);
    assert(!ble_midi_out_room(TO_BLE_DEV)); ble_midi_tap(pkt,TO_BLE);
    plat_ble_enable(0); assert(!plat_ble_connected() && !ble_midi_out_room(TO_BLE)); ble_poll();
    assert(plat_ble_state()==BLE_OFF && !plat_ble_linked() && off_calls==1 && !open_log.active);
    plat_ble_enable(1); assert(plat_ble_state()==BLE_STARTING); ble_poll();
    assert(plat_ble_state()==BLE_READY && start_calls==1 && on_calls==1 && open_log.active);
    for(unsigned i=0;i<5;i++) {
        link=subscribed=1;
        plat_ble_enable(0); ble_poll(); assert(!plat_ble_linked());
        plat_ble_enable(1); ble_poll(); assert(plat_ble_state()==BLE_READY && start_calls==1);
    }
    for(unsigned i=0;i<60;i++) plat_log("ring line");
    assert(plat_ble_log_total()>=60 && !strcmp(plat_ble_log_line(47),"ring line") && !*plat_ble_log_line(48));
    unsigned total=plat_ble_log_total(); reboot(0); assert(!radio_last_bad && plat_ble_log_total()==total);
    plat_ble_start(); ble_poll(); crashes++; reboot(0);
    assert(radio_last_bad && plat_ble_state()==BLE_OFF);
    /* The main boot gate respects a prior Bluetooth crash and APP_BLE_AT_BOOT;
     * explicit start still works. A cold boot clears the crash inhibition. */
    if(!radio_last_bad) ble_start(); assert(!ble_wanted);
    plat_ble_start(); ble_poll(); assert(plat_ble_state()==BLE_READY);
    radio_clean_exit(); crashes++; reboot(0); assert(!radio_last_bad);
    open_log.active=1; fm1_boot.p3_rst=4; radio_boot_note(); assert(radio_last_bad);
    open_log.active=1; reboot(1); assert(!radio_last_bad);
    /* A flash job's hold: Bluetooth off without the app's wish changing, the reason by job, back as wished at the
     * end (also a wish changed meanwhile); a link that does not end: -8 after FLASH_QUIET_MS; a backup's save ends a
     * link only while it is on; not started: never started meanwhile. */
    {
        uint32_t offs=off_calls,ons=on_calls,t0;
        plat_ble_start(); ble_poll(); assert(plat_ble_state()==BLE_READY);
        link=subscribed=1;
        assert(!flash_quiet_begin(FLASH_QUIET_UPDATE) && off_reason==0x15 && !link && off_calls==offs+1);
        assert(ble_wanted && plat_ble_state()==BLE_OFF && !plat_ble_connected());
        for(unsigned i=0;i<4;i++) ble_poll();
        assert(on_calls==ons && plat_ble_state()==BLE_OFF);
        assert(!ble_open_store_begin()); ble_open_store_end(); assert(on_calls==ons);
        flash_quiet_end(); assert(off_reason==0x13 && on_calls==ons+1 && plat_ble_state()==BLE_READY);
        flash_quiet_end(); assert(on_calls==ons+1);
        assert(!flash_quiet_begin(FLASH_QUIET_RESTORE) && off_reason==0x13);
        plat_ble_enable(0); flash_quiet_end(); assert(plat_ble_state()==BLE_OFF && on_calls==ons+1);
        plat_ble_enable(1); ble_poll(); assert(plat_ble_state()==BLE_READY && on_calls==ons+2);
        link=subscribed=1; sticky=1; t0=fm1_ms;
        assert(flash_quiet_begin(FLASH_QUIET_RESTORE)==-8 && fm1_ms-t0>=FLASH_QUIET_MS && fm1_ms-t0<FLASH_QUIET_MS+2u);
        assert(ble_open_store_begin()==-8);
        sticky=0; flash_quiet_end(); assert(plat_ble_state()==BLE_READY);
        link=subscribed=1; offs=off_calls;
        assert(ble_open_store_begin()==-8 && off_calls==offs && link);            /* an ordinary save: link kept */
        flash_quiet_saves(1); assert(!ble_open_store_begin() && !link && off_calls==offs+1);
        ble_open_store_end(); assert(plat_ble_state()==BLE_OFF);
        assert(!ble_open_store_begin()); ble_open_store_end();                   /* (more saves: still quiet) */
        flash_quiet_saves(0); assert(plat_ble_state()==BLE_READY && off_reason==0x13);
        flash_quiet_saves(1); assert(!ble_open_store_begin()); ble_open_store_end(); flash_quiet_saves(0);
        assert(off_calls==offs+1);                                               /* not linked: nothing ended */
        ble_started=0; ble_applied=0; ble_state=BLE_OFF; plat_ble_start(); unsigned starts=start_calls;
        assert(!flash_quiet_begin(FLASH_QUIET_UPDATE)); ble_poll(); ble_poll();
        assert(start_calls==starts && plat_ble_state()==BLE_OFF);
        flash_quiet_end(); assert(start_calls==starts+1 && plat_ble_state()==BLE_READY);
        puts("open peripheral seam: flash-job hold (reason by job, wish kept, bounded, backup-save link end, no start meanwhile): PASS");
        reboot(1);
    }
    start_fail=1; plat_ble_start(); ble_poll(); assert(plat_ble_state()==BLE_FAILED && !ble_started);
    puts("open peripheral seam: OFF/STARTING/READY/FAILED, linked/subscribed, off/on, queues, crash gate, clean reset and retained log: PASS");
    return 0;
}
