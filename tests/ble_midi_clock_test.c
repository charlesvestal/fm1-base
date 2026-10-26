/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../open_ble/ble_midi.h"
#define CLOCK 0xf80fu
/* Connection events deliver quantized arrivals; sender clocks at 120/140 BPM
 * run independently. Include DAW future timestamps and both uptime wraps. */
static void evenly(unsigned event_us,unsigned bpm,uint32_t start,unsigned future)
{
    ble_midi_clock_t s={0};
    uint32_t previous_due=0,previous_sent=0;
    unsigned minimum=1000,maximum=0;
    for(unsigned i=0;i<1600;i++) {
        uint32_t sent_us=(uint32_t)((uint64_t)i*60000000u/(bpm*24u));
        uint32_t arrival_us=((sent_us+event_us-1u)/event_us)*event_us;
        uint32_t sent=start+sent_us/1000u;
        uint32_t now=start+arrival_us/1000u;
        uint16_t ts=(uint16_t)((sent+2937u+future)&8191u);
        uint32_t due=ble_midi_release(&s,CLOCK,ts,now);
        assert(due-now<=120u);
        if(i>200) {
            /* Once the link has shown all phases, de-jittered tick spacing is
             * the sender's 1-ms resolution, despite event batching. */
            unsigned gap=due-previous_due;
            assert(gap==sent-previous_sent);
            if(gap<minimum) minimum=gap;
            if(gap>maximum) maximum=gap;
        }
        previous_due=due; previous_sent=sent;
    }
    ble_midi_clock_diag_t d; ble_midi_clock_read(&s,&d);
    assert(d.ticks==1600 && d.gap_min==60000u/(bpm*24u));
    assert(d.gap_max==d.gap_min+1u && !d.gap_zero);
    printf("clock %u us events, %u BPM, future %u: %u..%u ms ticks PASS\n",event_us,bpm,future,minimum,maximum);
}
static uint16_t stamps[3];
static uint32_t packets[3];
static unsigned received;
static void message(uint32_t pkt,uint16_t ts,void *arg)
{
    (void)arg; assert(received<3); packets[received]=pkt; stamps[received++]=ts;
}
int main(void)
{
    ble_midi_usb_t decoder={0};
    ble_midi_usb_byte(&decoder,0x90,100,message,0);
    ble_midi_usb_byte(&decoder,60,100,message,0);
    ble_midi_usb_byte(&decoder,0xf8,101,message,0);
    ble_midi_usb_byte(&decoder,100,101,message,0);
    assert(received==2 && packets[0]==CLOCK && stamps[0]==101);
    assert(packets[1]==0x643c9009 && stamps[1]==100);
    const unsigned events[]={7500,15000,30000};
    for(unsigned i=0;i<3;i++) for(unsigned bpm=120;bpm<=140;bpm+=20)
        for(unsigned future=0;future<=100;future+=100)
            evenly(events[i],bpm,0xffffe000u,future);
    ble_midi_clock_t s={0};
    assert(ble_midi_release(&s,CLOCK,8190,1000)==1000);
    assert(ble_midi_release(&s,CLOCK,3,1015)==1015); /* wrap, +10 ms jitter */
    assert(ble_midi_release(&s,CLOCK,23,1025)==1035);
    assert(ble_midi_release(&s,0xfa0f,24,1026)==1036);
    assert(ble_midi_release(&s,0xfb0f,25,1027)==1037);
    assert(ble_midi_release(&s,0xfc0f,26,1028)==1038);
    /* A late sample may not hold a new tick more than 120 ms. */
    assert(ble_midi_release(&s,CLOCK,28,1500)==1500);
    assert(s.diag.allowed==120 && s.diag.late_max==470);
    assert(ble_midi_release(&s,CLOCK,498,1500)==1620);
    assert(ble_midi_release(&s,0x643c9009,498,1500)==1500);
    assert(ble_midi_release(&s,0xfe0f,498,1500)==1500);
    assert(ble_midi_release(&s,0x03020104,498,1500)==1500);
    /* Recent lateness ages out; silence reacquires phase without a false hold. */
    ble_midi_release(&s,CLOCK,1498,2500);
    ble_midi_release(&s,CLOCK,2498,3500);
    assert(ble_midi_release(&s,CLOCK,3498,4500)==4500);
    assert(ble_midi_release(&s,CLOCK,1,10000)==10000);
    ble_midi_clock_diag_t d; ble_midi_clock_read(&s,&d); assert(d.ticks && !s.diag.ticks);
    puts("clock wrap, 120-ms cap, immediate non-transport, aging, silence: PASS");
    return 0;
}
