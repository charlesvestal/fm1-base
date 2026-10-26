/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* BLE-MIDI 13-bit millisecond timestamps, packet-local running status, and
 * streaming SysEx. No heap; MIDI realtime does not disturb a pending message. */
#include "ble_midi.h"
static unsigned msg_size(uint8_t status)
{
    if(status<0x80) return 0;
    if(status<0xf0) return (status&0xe0)==0xc0 ? 2u : 3u;
    if(status==0xf1 || status==0xf3) return 2;
    if(status==0xf2) return 3;
    if(status==0xf6 || status>=0xf8) return 1;
    return 0;
}
static uint16_t stamp(uint16_t *high,uint8_t *last,uint8_t low,int *seen)
{
    low&=127;
    if(*seen && low<*last) *high=(uint16_t)((*high+128u)&0x1f80u);
    *last=low; *seen=1; return (uint16_t)(*high|low);
}
static int decode(ble_midi_decoder_t *s,const uint8_t *p,unsigned n,ble_midi_byte_fn emit,void *arg)
{
    unsigned i=1,need=0;
    uint8_t running=0,last=0;
    uint16_t high,ts=0;
    int seen=0,boundary=1;
    if(n<2 || (p[0]&0xc0u)!=0x80u) return 0;
    high=(uint16_t)((p[0]&63u)<<7);
    while(i<n) {
        uint8_t b=p[i++];
        if(s->sysex) {
            if(b&0x80u) {
                ts=stamp(&high,&last,b,&seen);
                if(i==n) return 0;
                b=p[i++];
                if(b!=0xf7 && b<0xf8) return 0;
            } else if(!seen) ts=high;
            if(emit) emit(b,ts,arg);
            if(b==0xf7) { s->sysex=0; boundary=1; running=0; }
            continue;
        }
        if(boundary || (need && (b&0x80u))) {
            if(!(b&0x80u) && (!boundary || !running)) return 0;
            if(b&0x80u) {
                ts=stamp(&high,&last,b,&seen);
                if(i==n) return 0;
                b=p[i++];
            }
            boundary=0;
        }
        if(b>=0xf8) {
            if(emit) emit(b,ts,arg);
            if(!need) boundary=1;
            continue;
        }
        if(b==0xf0) {
            if(need) return 0;
            s->sysex=1; running=0;
            if(emit) emit(b,ts,arg);
            continue;
        }
        if(b&0x80u) {
            unsigned size=msg_size(b);
            if(!size || need) return 0;
            running=b<0xf0 ? b : 0; need=size-1u;
            if(emit) emit(b,ts,arg);
            if(!need) boundary=1;
        } else {
            if(!need) {
                if(!running) return 0;
                need=msg_size(running)-1u;
                if(emit) emit(running,ts,arg);
            }
            if(emit) emit(b,ts,arg);
            if(!--need) boundary=1;
        }
    }
    return need==0;
}
int ble_midi_decode(ble_midi_decoder_t *s,const uint8_t *p,unsigned n,ble_midi_byte_fn emit,void *arg)
{
    ble_midi_decoder_t copy=*s;
    if(!decode(&copy,p,n,0,0)) return 0;
    return decode(s,p,n,emit,arg);
}
int ble_midi_encode_usb(uint32_t packet,uint32_t ms,uint8_t *p,unsigned cap)
{
    static const uint8_t len[16]={0,0,2,3,3,1,2,3,3,3,3,3,2,2,3,1};
    unsigned cin=packet&15u,n=len[cin],i,at=1;
    int sysex=cin==4 || cin==6 || cin==7 || (cin==5 && ((packet>>8)&255u)==0xf7);
    uint8_t first=(uint8_t)(packet>>8),low=(uint8_t)(0x80u|(ms&127u));
    if(!n || cap<7) return 0;
    p[0]=(uint8_t)(0x80u|((ms>>7)&63u));
    if(!sysex || first==0xf0) p[at++]=low;
    for(i=0;i<n;i++) {
        uint8_t b=(uint8_t)(packet>>(8u*(i+1u)));
        if(sysex && b==0xf7) p[at++]=low;
        p[at++]=b;
    }
    return (int)at;
}
void ble_midi_usb_byte(ble_midi_usb_t *s,uint8_t b,uint16_t ts,ble_midi_usb_fn emit,void *arg)
{
    uint32_t packet; unsigned i;
    if(b>=0xf8) { emit(15u|((uint32_t)b<<8),ts,arg); return; }
    if(b==0xf0) { s->sysex=1; s->sx_n=0; s->n=0; s->status=0; }
    if(s->sysex) {
        if(!s->sx_n) s->timestamp=ts;
        s->sx[s->sx_n++]=b;
        if(s->sx_n==3 || b==0xf7) {
            packet=b==0xf7 ? 4u+s->sx_n : 4u;
            for(i=0;i<s->sx_n;i++) packet|=(uint32_t)s->sx[i]<<(8u*(i+1u));
            emit(packet,s->timestamp,arg); s->sx_n=0;
        }
        if(b==0xf7) s->sysex=0;
        return;
    }
    if(b&0x80u) { s->timestamp=ts; s->status=b; s->need=(uint8_t)msg_size(b); s->n=0; }
    if(!s->need) return;
    s->bytes[s->n++]=b;
    if(s->n==s->need) {
        packet=s->status<0xf0 ? s->status>>4 : s->need==3 ? 3u : s->need==2 ? 2u : 5u;
        for(i=0;i<s->n;i++) packet|=(uint32_t)s->bytes[i]<<(8u*(i+1u));
        emit(packet,s->timestamp,arg); s->n=0; s->need=0;
    }
}

/* Signed modulo-13 distance. Subtract unsigned first: valid across both
 * sender timestamp wrap and our uint32_t millisecond uptime wrap. */
static int32_t clock_distance(uint32_t d)
{
    d&=8191u;
    return d<4096u ? (int32_t)d : (int32_t)d-8192;
}
int ble_midi_transport(uint32_t packet)
{
    uint8_t b=(uint8_t)(packet>>8);
    return b==0xf8 || b==0xfa || b==0xfb || b==0xfc;
}
uint32_t ble_midi_release(ble_midi_clock_t *s,uint32_t packet,uint16_t ts,uint32_t now)
{
    int32_t late;
    uint32_t elapsed,allow;
    if(!ble_midi_transport(packet)) return now;
    ts&=8191u;
    /* After silence longer than half a timestamp cycle there is no unambiguous
     * phase. Reacquire rather than interpreting an old clock as future data. */
    if(!s->initialized || now-s->last_arrival>=4096u) {
        s->offset=(uint16_t)((now-ts)&8191u);
        s->bucket_at=now; s->peak=s->previous_peak=0;
        s->initialized=1; s->have_tick=0;
    }
    elapsed=now-s->bucket_at;
    if(elapsed>=1000u) {
        s->previous_peak=elapsed<2000u ? s->peak : 0;
        s->peak=0; s->bucket_at=now;
    }
    late=clock_distance(now-ts-s->offset);
    if(late<0) {
        /* A quicker arrival moves the phase back. Keep the recent allowance
         * relative to that new minimum, including already observed jitter. */
        uint32_t shift=(uint32_t)-late;
        s->peak=(uint16_t)(s->peak+shift>120u ? 120u : s->peak+shift);
        s->previous_peak=(uint16_t)(s->previous_peak+shift>120u ? 120u : s->previous_peak+shift);
        s->offset=(uint16_t)((now-ts)&8191u); late=0;
    }
    if((uint32_t)late>s->diag.late_max) s->diag.late_max=(uint32_t)late;
    allow=(uint32_t)late>120u ? 120u : (uint32_t)late;
    if(allow>s->peak) s->peak=(uint16_t)allow;
    allow=s->peak>s->previous_peak ? s->peak : s->previous_peak;
    s->diag.allowed=allow;
    if((uint8_t)(packet>>8)==0xf8) {
        if(s->have_tick) {
            uint32_t gap=(ts-s->last_ts)&8191u;
            if(!s->gaps++ || gap<s->diag.gap_min) s->diag.gap_min=gap;
            if(gap>s->diag.gap_max) s->diag.gap_max=gap;
            if(!gap) s->diag.gap_zero++;
        }
        s->last_ts=ts; s->have_tick=1; s->diag.ticks++;
    }
    s->last_arrival=now;
    /* Late outliers beyond the cap are due immediately. */
    return (uint32_t)late>allow ? now : now+allow-(uint32_t)late;
}
void ble_midi_clock_read(ble_midi_clock_t *s,ble_midi_clock_diag_t *out)
{
    *out=s->diag;
    out->allowed=s->peak>s->previous_peak ? s->peak : s->previous_peak;
    if(!s->gaps) out->gap_min=0;
    s->gaps=0;
    s->diag=(ble_midi_clock_diag_t){0};
}
