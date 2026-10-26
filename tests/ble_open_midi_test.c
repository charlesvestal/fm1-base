/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
/* Compiles the actual platform MIDI functions, extracted by the runner. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "../include/plat.h"
#define RING_PUBLISH() do {} while(0)
#define MQ 8u
static uint32_t now,mi_w,mi_r,midi_in_q[MQ],usb_room,bt_room,last_usb,last_bt,bt_in;
static uint8_t midi_usb_lost,midi_in_source[MQ];
static int in_tick,bt_connected,usb_host_ready;
static struct { int config,suspended; } usb;
uint32_t plat_ms(void) { return now; }
uint32_t plat_lock(void) { return 0; }
void plat_unlock(uint32_t f) { (void)f; }
int plat_ble_connected(void) { return bt_connected; }
static int usb_midi_out_ready(void) { return usb.config || usb_host_ready; }
static uint32_t midi_out_room(void) { return usb_room; }
static uint32_t midi_out_link(void) { return 7; }
static uint32_t ble_midi_out_room(uint32_t to) { return (to&TO_BLE_HOST) ? bt_room : 0; }
static void midi_out_event(uint32_t p) { last_usb=p; }
static void ble_midi_tap(uint32_t p,uint32_t to) { if(to&TO_BLE_HOST) last_bt=p; }
static int ble_midi_in(uint32_t *p,uint32_t *from) { if(!bt_in) return 0; *from=TO_BLE_HOST; *p=bt_in; bt_in=0; return 1; }
#include "midi_seam.inc"
int main(void)
{
    uint32_t p; int port;
    assert(!plat_midi_out_room(TO_USB|TO_BLE));
    usb.config=1; usb_room=23; assert(plat_midi_out_room(TO_USB|TO_BLE)==23);
    bt_connected=1; bt_room=3; assert(plat_midi_out_room(TO_USB|TO_BLE)==3);
    bt_room=0; assert(!plat_midi_out_room(TO_USB|TO_BLE));
    bt_room=3; usb_room=0; assert(!plat_midi_out_room(TO_USB|TO_BLE));
    usb.suspended=1; assert(plat_midi_out_room(TO_USB|TO_BLE)==3);
    assert(!plat_midi_out_room(TO_BLE_DEV));
    assert(plat_midi_out_room(TO_BLE_HOST)==3);
    last_bt=0; plat_midi_send(0x643d9009,TO_BLE_DEV);
    assert(!last_bt && !plat_midi_count(PORT_BLE,1));
    usb.suspended=0; usb.config=0; usb_host_ready=1; usb_room=2;
    assert(plat_midi_out_room(TO_USB)==2 && plat_midi_out_room(TO_BLE_DEV|TO_USB)==2);
    usb_room=0; assert(!plat_midi_out_room(TO_USB|TO_BLE));
    now=123; plat_midi_send(0x643c9009,TO_USB|TO_BLE);
    assert(last_usb==0x643c9009 && last_bt==last_usb);
    assert(plat_midi_count(PORT_BLE,1)==1 && plat_midi_ago(PORT_BLE,1)==0);
    plat_midi_send(0x0000f80f,TO_BLE); assert(plat_midi_count(PORT_BLE,1)==1);
    bt_in=0x643c9009; assert(plat_midi_in(&p,&port) && port==PORT_BLE && p==0x643c9009);
    assert(plat_midi_in_from()==TO_BLE_HOST);
    assert(plat_midi_count(PORT_BLE,0)==1 && !plat_midi_in(&p,&port));
    midi_in_source[mi_w%MQ]=2; midi_in_q[mi_w++%MQ]=0x643c9019;
    assert(plat_midi_in(&p,&port) && port==PORT_TRS && p==0x643c9009);
    assert(plat_midi_in_from()==0);
    midi_in_source[mi_w%MQ]=1; midi_in_q[mi_w++%MQ]=0x643d9019; /* USB host cable 1 */
    assert(plat_midi_in(&p,0) && p==0x643d9009 && plat_midi_in_from()==TO_USB);
    last_bt=last_usb=0;
    plat_midi_send(p,(TO_USB|TO_BLE)&~plat_midi_in_from());
    assert(last_bt==p && !last_usb);
    midi_usb_lost=1; assert(plat_usb_lost() && !plat_usb_lost());
    now=131; assert(plat_midi_ago(PORT_BLE,0)==8);
    puts("actual platform MIDI: mixed/full/absent queues, USB/BLE packets, input port, counters/realtime and activity: PASS");
    return 0;
}
