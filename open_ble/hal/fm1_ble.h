/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wall"
#pragma clang diagnostic error "-Wextra"
/* BLE DMA/baseband HAL: frozen r3 02 §§2-7,9 and 04 §0. */
#pragma once
#include "fm1_ble_radio.h"
#include "../ble_link.h"
#include "../ble_breadcrumb.h"
#include "../ble_timing.h"
#include "../ble_health.h"
#if FELUCCA_BLE_S31
#include "../ble_privacy.h"
#endif
#if FELUCCA_BLE_STAGE >= 3
#include "../ble_security.h"
#include "../ble_seed.h"
#endif

/* External linkage prevents the compiler treating warm-reset RAM as zero.
 * This record is never cleared by HCI Reset or diagnostic counter resets. */
volatile ble_breadcrumb_t fm1_ble_breadcrumb __attribute__((section(".noinit")));

/* Measure only the controller's own outer critical sections. BSP blackouts
 * and the cumulative 390 us/920 us limit still require the PB3.1 bench probe. */
static uint32_t ble_critical_depth,ble_critical_start;
static uint32_t fm1_ble_irq_save(void)
{
    uint32_t state=fm1_icfg();
    fm1_icfg_set(state&~0x100u); br_sync();
    if (!ble_critical_depth++) ble_critical_start=fm1_ticks();
    return state;
}
static void fm1_ble_irq_restore(uint32_t state)
{
    if (!--ble_critical_depth) {
        uint32_t dt=fm1_ticks()-ble_critical_start;
        if (dt>br_diag.critical_max_ticks) br_diag.critical_max_ticks=dt;
        if (dt>390u*24u) br_diag.critical_over_390us++;
    }
    br_sync(); fm1_icfg_set(state); br_sync();
}
static inline uint32_t fm1_ble_icfg_read(void) { return fm1_icfg(); }
#if FELUCCA_BLE_STAGE >= 3
static inline void fm1_ble_icfg_write(uint32_t state) { br_sync(); fm1_icfg_set(state); br_sync(); }
#endif
static void fm1_ble_fail(uint32_t code) { br_error(code); }
static void fm1_ble_line_enable(unsigned n)
{
    FM1_ICFG(n)|=1u<<((n&7u)*4u);
}
static struct {
    uint8_t bb[BLE_BB_SIZE];
    uint32_t modem[243],canary[4];
} ble_dma __attribute__((section(".ble_dma"),aligned(4)));
static struct {
    ble_adv_t adv;
    uint8_t frame_template[BLE_FRAME_SIZE];
    uint32_t stop_start,stop_bound,last_worker,last_canary;
    volatile uint8_t state,restart;
    int8_t tx_id[2];
    uint8_t refill_busy;
    uint8_t start_phase;
    uint16_t start_offset;
    uint32_t rx_stamp[2]; /* t_free: never changed while a slot remains full */
    uint8_t rx_delayed[2];
#if FM1B_BLE_RELIABILITY
    uint8_t rx_pinned[2];
#endif
} bb;
enum { BB_FRAME=16, BB_RX0=340, BB_RX1=596, BB_TX0=852, BB_TX1=892 };
#define BB_F16(off) (*(volatile uint16_t *)(void *)(ble_dma.bb+BB_FRAME+(off)))
#define BB_F8(off) (*(volatile uint8_t *)(void *)(ble_dma.bb+BB_FRAME+(off)))
#define BB_F32(off) (*(volatile uint32_t *)(void *)(ble_dma.bb+BB_FRAME+(off)))

#if FM1B_BLE_RELIABILITY
int ble_port_diag_radio_idle(void) { return !(BR_R(0x28038u)&2u); }
int ble_port_diag_rx_read(unsigned slot,uint16_t *header,uint16_t *status,uint8_t out[31])
{
    if(slot>=2 || !bb.rx_pinned[slot] || !(BB_F8(BF_RX_CTL+slot)&1u)) return 0;
    *header=BB_F16(BF_RX_DATA+2*slot); *status=BB_F16(BF_RX_STAT+2*slot);
    unsigned n=*header>>8; if(n>31) return 0;
    volatile const uint8_t *p=ble_dma.bb+(slot ? BB_RX1 : BB_RX0);
    br_sync(); for(unsigned i=0;i<n;i++) out[i]=p[i]; br_sync(); return 1;
}
void ble_port_diag_rx_release(unsigned slot)
{
    if(slot>=2 || !bb.rx_pinned[slot]) return;
    bb.rx_pinned[slot]=0;
    BB_F16(BF_RX_PTR+2*slot)=(uint16_t)(slot ? BB_RX1 : BB_RX0);
    br_sync(); BB_F8(BF_RX_CTL+slot)&=(uint8_t)~1u; br_sync();
    bb.rx_stamp[slot]=fm1_ticks(); bb.rx_delayed[slot]=0;
}
#endif
static inline void bb_mark(uint8_t stage,uint16_t detail)
{
    ble_breadcrumb_mark(&fm1_ble_breadcrumb,stage,bb.state,detail,fm1_ticks());
}
static inline void bb_mark_event(uint8_t stage) { bb_mark(stage,BB_F16(BF_EVT)); }
static inline void bb_window_length(uint32_t us)
{
    bb_mark(BLE_STAGE_WINDOW,(uint16_t)us);
    ble_frame_window_set((volatile uint16_t *)(void *)(ble_dma.bb+BB_FRAME),us);
}
static void bb_anchor(unsigned id,unsigned index,uint32_t value)
{
    bb_mark(BLE_STAGE_ANCHOR,(uint16_t)((id<<8)|index));
    uint32_t k=fm1_ble_irq_save();
    BR_R(0x28020u)=value; br_sync(); BR_R(0x2801cu)=(index<<10)|(id<<4)|5;
    fm1_ble_irq_restore(k);
}
static uint32_t bb_anchor_read(unsigned index)
{
    uint32_t k=fm1_ble_irq_save(),v;
    BR_R(0x2801cu)=(index<<10)|2u; br_sync(); v=BR_R(0x28024u);
    fm1_ble_irq_restore(k); return v;
}
static void bb_map(const uint8_t *map)
{
    unsigned i,n=0;
    for(i=0;i<40;i++) {
        BB_F8(BF_FREQ_IDX+i)=(uint8_t)i; BB_F8(BF_FREQ_TBL+i)=ble_frequency((uint8_t)i);
        if(i>=37 || (map[i/8]&(1u<<(i%8)))) {
            unsigned j=i>=37 ? i : n++;
            BB_F8(BF_FREQ_USED+j)=(uint8_t)i;
            BB_F8(BF_FREQ_USED_TBL+j)=ble_frequency((uint8_t)i);
        }
    }
    BB_F16(BF_CHMAP)=ble_get16(map); BB_F16(BF_CHMAP+2)=ble_get16(map+2);
    BB_F16(BF_CHMAP+4)=(uint16_t)((n<<5)|(map[4]&31u)); br_sync();
}
static void bb_refill_limit(unsigned limit)
{
    uint32_t k=fm1_ble_irq_save(),start=fm1_ticks(),ccm_before=ble_time_ccm_total();
    unsigned a,b,j,first,tries=0,loads=0;
    if(bb.refill_busy
#if FM1B_BLE_RELIABILITY
       || ble_reliability_failed()
#endif
       || (bb.state!=4 && bb.state!=2)) { fm1_ble_irq_restore(k); return; }
    bb.refill_busy=1;
    do {
        a=(BB_F16(BF_TX_TOGGLE)&1u)|((BB_F8(BF_TX_CTL)&1u)<<1)|((BB_F8(BF_TX_CTL+1)&1u)<<2);
        b=(BB_F16(BF_TX_TOGGLE)&1u)|((BB_F8(BF_TX_CTL)&1u)<<1)|((BB_F8(BF_TX_CTL+1)&1u)<<2);
        if(++tries==4 && a!=b) {
            bb.refill_busy=0; ble_time_refill_end(start,ccm_before); fm1_ble_irq_restore(k); ble_port_pend(); return;
        }
    } while(a!=b);
    first=a&1u;
    for(j=0;j<2;j++) {
        unsigned slot=first^j; int id; uint8_t n,llid; unsigned md;
        if(!(a&(2u<<slot))) continue;
#if FELUCCA_BLE_STAGE >= 3
        if(bb.tx_id[slot]>=0 && ble_ll_tx_boundary((unsigned)bb.tx_id[slot])) continue;
#endif
        if(bb.tx_id[slot]>=0) {
#if FELUCCA_BLE_STAGE >= 3
            ble_radio_tx(BLE_RADIO_ACK,BB_F16(BF_EVT),slot,bb.tx_id[slot],BB_F16(BF_TX_DATA+2*slot),1);
#endif
            id=bb.tx_id[slot]; bb.tx_id[slot]=-1; ble_ll_tx_ack((unsigned)id); }
        if(bb.state!=4 || ble_link.state>=BLE_LINK_CLOSING) continue;
        if(loads==limit) continue;
        id=ble_ll_tx_pick();
        if(id<0) { BB_F16(BF_FLOW)&=(uint16_t)~64u; continue; }
        n=ble_ll_tx_len((unsigned)id); llid=ble_ll_tx_llid((unsigned)id); md=(unsigned)ble_ll_acl_waiting();
#if FELUCCA_BLE_STAGE >= 3
        ble_radio_tx(BLE_RADIO_PREP,BB_F16(BF_EVT),slot,id,BB_F16(BF_TX_DATA+2*slot),
                     (uint16_t)(BB_F8(BF_TX_CTL+slot)|(BB_F16(BF_TX_TOGGLE)<<8)));
        if(!ble_ll_tx_prepare((unsigned)id,ble_dma.bb+(slot ? BB_TX1 : BB_TX0),&n)) break;
#else
        memcpy(ble_dma.bb+(slot ? BB_TX1 : BB_TX0),ble_ll_tx_data((unsigned)id),n);
#endif
        BB_F16(BF_TX_PTR+slot*2)=(uint16_t)(slot ? BB_TX1 : BB_TX0);
        BB_F16(BF_TX_ADV+slot*2)=0;
        BB_F16(BF_TX_DATA+slot*2)=(uint16_t)((BB_F16(BF_TX_DATA+slot*2)&4u)|((uint16_t)n<<8)|(md<<3)|llid);
        BB_F16(BF_FLOW)=(uint16_t)((BB_F16(BF_FLOW)&~64u)|(md<<6));
#if FELUCCA_BLE_STAGE >= 3
        ble_security_tx_loaded((unsigned)id,(BB_F16(BF_TX_DATA+slot*2)>>2)&1u);
#endif
#if FELUCCA_BLE_STAGE >= 3
        ble_radio_tx(BLE_RADIO_LOAD,BB_F16(BF_EVT),slot,id,BB_F16(BF_TX_DATA+2*slot),
                     (uint16_t)(BB_F8(BF_TX_CTL+slot)|(BB_F16(BF_TX_TOGGLE)<<8)));
#endif
        bb.tx_id[slot]=(int8_t)id; br_sync(); BB_F8(BF_TX_CTL+slot)&=(uint8_t)~1u; br_sync();
        loads++;
    }
    bb.refill_busy=0;
    ble_time_refill_end(start,ccm_before);
    fm1_ble_irq_restore(k);
}
static void bb_refill(void) { bb_refill_limit(2); }
static void bb_connection(const ble_conn_t *c,uint8_t header,uint32_t now)
{
    if(!ble_ll_create(c,now)) { br_diag.connect_ignored++; return; }
    ble_health_connect(1);
    bb_mark(BLE_STAGE_CONNECT,c->interval);
    unsigned slot,t=BB_F16(BF_TX_TOGGLE)&1u;
    uint16_t w=ble_widen(c);
    for(slot=0;slot<2;slot++) {
        BB_F16(BF_TX_ADV+slot*2)=(uint16_t)(((header>>7)&1u)<<4);
        BB_F16(BF_TX_DATA+slot*2)=(uint16_t)(((slot==0 ? t : !t)<<2)|1u);
        bb.tx_id[slot]=-1; bb.rx_delayed[slot]=0;
#if FM1B_BLE_RELIABILITY
        bb.rx_pinned[slot]=0;
#endif
    }
    bb.refill_busy=0;
    BB_F16(BF_PRIO_CTRL)=28; BB_F16(BF_PRIO_STAT)=28;
    BB_F16(0)=0x8000; BB_F16(BF_TX_TOGGLE)&=(uint16_t)~2u; BB_F16(BF_RX_TOGGLE)=0;
    BB_F16(10)=(uint16_t)c->aa; BB_F16(12)=(uint16_t)(c->aa>>16);
    BB_F16(BF_CRC)=(uint16_t)c->crc; BB_F16(BF_CRC+2)=(uint16_t)(c->crc>>16);
    BB_F16(BF_OPTIONS)=0x1c2c; BB_F16(BF_FLOW)=(uint16_t)((BB_F16(BF_FLOW)&~48u)|16u);
    BB_F8(BF_TX_CTL)|=1; BB_F8(BF_TX_CTL+1)|=1;
    BB_F16(0)=0x8004;
    bb_anchor(0,5,0);
    bb_anchor(0,4,c->offset ? ((2u*c->offset-1u)|0x8000u) : 0u);
    /* Latency is held disabled for the first six events. */
    bb_anchor(0,2,0x7000u|c->latency);
    BB_F16(72)=w/625u; BB_F16(74)=(uint16_t)(0x5000u|(w%625u));
    bb_anchor(0,8,0); bb_anchor(0,3,0); BB_F16(BF_EVT)=0;
    bb_window_length((uint32_t)c->win*1250u+BLE_FIRST_WINDOW_EXTRA_US); BB_F16(BF_WIN_STEADY)=50;
    bb_anchor(0,1,c->interval*2u); bb_anchor(0,15,0x8000);
    bb_map(c->map); bb_anchor(0,6,0x8000u|((uint32_t)c->hop<<8)|c->hop);
    for(slot=0;slot<2;slot++) {
        BB_F16(BF_TX_DATA+2*slot)=(uint16_t)((slot!=t ? 4u : 0u)|1u);
        BB_F8(BF_RX_CTL+slot)&=(uint8_t)~1u; bb.rx_stamp[slot]=fm1_ticks();
    }
    bb_mark(BLE_STAGE_CREATE,0);
    bb.state=4; br_sync();
    bb_mark(BLE_STAGE_CONNECTED,0);
}
#if FELUCCA_BLE_STAGE >= 3
static void bb_connected_rx(uint32_t stamp,int recovery)
{
#if FM1B_BLE_RELIABILITY
    if(ble_reliability_failed()) return;
#endif
    unsigned a,b,j,first,held=0,slots[2],n[2],position=2;
    uint16_t status[2],header[2],event=BB_F16(BF_EVT);
    uint8_t copy[2][31],marker=0; int boundary_slot=-1;
    uint32_t older=0; int has_older=0;
    /* Boundary completion is observed before the held-packet snapshot, and
     * is never settled by generic refill. ACK visibility remains P3-7. */
    for(j=0;j<2;j++) if(bb.tx_id[j]>=0 && (BB_F8(BF_TX_CTL+j)&1u)) {
        uint8_t m=ble_ll_tx_boundary((unsigned)bb.tx_id[j]);
        if(m) {
            if(marker) { ble_ll_close(0x3d); return; }
            marker=m; boundary_slot=(int)j;
        }
    }
    do {
        a=(BB_F16(BF_RX_TOGGLE)&1u)|((BB_F8(BF_RX_CTL)&1u)<<1)|((BB_F8(BF_RX_CTL+1)&1u)<<2);
        b=(BB_F16(BF_RX_TOGGLE)&1u)|((BB_F8(BF_RX_CTL)&1u)<<1)|((BB_F8(BF_RX_CTL+1)&1u)<<2);
    } while(a!=b);
    first=a&1u;
    for(j=0;j<2;j++) {
        if(a&6u) ble_radio_tx(BLE_RADIO_TX_STATE,event,j,bb.tx_id[j],BB_F16(BF_TX_DATA+2*j),
                     (uint16_t)(BB_F8(BF_TX_CTL+j)|(BB_F16(BF_TX_TOGGLE)<<8)));
        if(a&(2u<<(first^j))) {
#if FM1B_BLE_RELIABILITY
            if(bb.rx_pinned[first^j]) continue;
#endif
            slots[held++]=first^j;
        }
    }
    if(marker && (!held || !ble_ll_rx_reserve(held,1))) {
        /* Reservation can block before the per-slot loop. Retain that wait
         * just like ordinary backlog pressure: later IRQ service must use
         * t_free, and diagnostics must not call the held packet prompt. */
        for(j=0;j<held;j++) bb.rx_delayed[slots[j]]=1;
        ble_port_pend(); return;
    }
    for(j=0;j<held;j++) {
        unsigned slot=slots[j];
        status[j]=BB_F16(BF_RX_STAT+2*slot); header[j]=BB_F16(BF_RX_DATA+2*slot); n[j]=header[j]>>8;
        if(n[j]<=31) memcpy(copy[j],ble_dma.bb+(slot ? BB_RX1 : BB_RX0),n[j]);
    }
    if(marker) {
        int id=bb.tx_id[boundary_slot];
        ble_radio_tx(BLE_RADIO_ACK,event,(unsigned)boundary_slot,id,BB_F16(BF_TX_DATA+2*boundary_slot),1);
        bb.tx_id[boundary_slot]=-1; ble_ll_tx_ack((unsigned)id);
        position=held==1 ? 0 : held;
        if(held==2) for(j=0;j<held;j++)
            if(ble_ll_rx_after_marker(marker,status[j],header[j],copy[j])) { position=j; break; }
    }
    ble_entropy_sample(0,BB_F32(316),0);
    for(j=0;j<held;j++) {
        unsigned slot=slots[j]; int delayed=recovery || bb.rx_delayed[slot] || held==2;
        uint8_t flags=(uint8_t)((bb.rx_delayed[slot] ? BLE_RX_PRESSURE : 0) |
            (held==2 ? BLE_RX_BOTH_FULL : 0) | (recovery ? BLE_RX_RECOVERY : 0));
        uint32_t ts=delayed ? bb.rx_stamp[slot] : stamp;
        if(marker && position==j) { if(!ble_ll_rx_marker(marker)) { ble_ll_close(0x3d); return; } marker=0; }
        if(delayed && has_older && (int32_t)(older-ts)>0) ts=older;
        if(!ble_ll_rx_room()) { bb.rx_delayed[slot]=1; continue; }
#if FM1B_BLE_RELIABILITY
        /* Keep every new nonempty slot owned until its ordered auth/decode.
         * Including clear S1 PDUs avoids guessing encryption across RX_ON. */
        if(!ble_rx_class(status[j],(uint8_t)n[j]) && n[j] && n[j]<=31) bb.rx_pinned[slot]=1;
        else
#endif
        {
            BB_F16(BF_RX_PTR+2*slot)=(uint16_t)(slot ? BB_RX1 : BB_RX0);
            br_sync(); BB_F8(BF_RX_CTL+slot)&=(uint8_t)~1u; br_sync();
            bb.rx_stamp[slot]=fm1_ticks(); bb.rx_delayed[slot]=0;
        }
        older=ts; has_older=1;
        ble_ll_rx_slot(status[j],header[j],copy[j],ts,event,delayed,(uint8_t)slot,flags);
    }
    if(marker && !ble_ll_rx_marker(marker)) ble_ll_close(0x3d);
    bb_refill();
}
#else
static void bb_connected_rx(uint32_t stamp,int recovery)
{
    unsigned first,a,b,j; uint32_t older=0; int has_older=0;
    do {
        a=(BB_F16(BF_RX_TOGGLE)&1u)|((BB_F8(BF_RX_CTL)&1u)<<1)|((BB_F8(BF_RX_CTL+1)&1u)<<2);
        b=(BB_F16(BF_RX_TOGGLE)&1u)|((BB_F8(BF_RX_CTL)&1u)<<1)|((BB_F8(BF_RX_CTL+1)&1u)<<2);
    } while(a!=b);
    first=a&1u;
    for(j=0;j<2;j++) {
        unsigned slot=first^j,n; uint16_t status,header,event; uint32_t ts; uint8_t copy[27]; int delayed;
        if(!(a&(2u<<slot))) continue;
        status=BB_F16(BF_RX_STAT+2*slot); header=BB_F16(BF_RX_DATA+2*slot); event=BB_F16(BF_EVT);
        delayed=recovery || bb.rx_delayed[slot] || (a&6u)==6u;
        ts=delayed ? bb.rx_stamp[slot] : stamp;
        if(delayed && has_older && (int32_t)(older-ts)>0) ts=older;
        if(!ble_ll_rx_room()) { bb.rx_delayed[slot]=1; continue; }
        n=header>>8;
        if(n<=sizeof copy) memcpy(copy,ble_dma.bb+(slot ? BB_RX1 : BB_RX0),n);
        bb.rx_delayed[slot]=0;
        BB_F16(BF_RX_PTR+2*slot)=(uint16_t)(slot ? BB_RX1 : BB_RX0);
        br_sync(); BB_F8(BF_RX_CTL+slot)&=(uint8_t)~1u; br_sync();
        bb.rx_stamp[slot]=fm1_ticks(); older=ts; has_older=1;
        /* Queue capacity was reserved by exclusion; hardware can now reuse
         * this fixed DMA buffer. Decode/probe only the independent snapshot. */
        ble_ll_rx_delayed(status,header,copy,ts,event,delayed);
    }
    bb_refill();
}
#endif
static void bb_zero_anchors(void)
{
    unsigned id,i;
    for (id=0;id<8;id++) for (i=0;i<17;i++) bb_anchor(id,i,0);
}
static void bb_remap(void)
{
    unsigned first=BB_F16(BF_TX_TOGGLE)&1u,j;
    for (j=0;j<2;j++) {
        unsigned slot=first^j;
        uint8_t n=(uint8_t)((j ? bb.adv.scan_len : bb.adv.data_len)+6);
        uint8_t type=(uint8_t)(j ? 4 : bb.adv.params.type==3 ? 2 : 0);
        BB_F16(BF_TX_PTR+2*slot)=(uint16_t)(j ? BB_TX1 : BB_TX0);
        BB_F16(BF_TX_ADV+2*slot)=ble_adv_header(type,bb.adv.params.own_type);
        BB_F16(BF_TX_DATA+2*slot)=(uint16_t)n<<8;
        br_sync(); BB_F8(BF_TX_CTL+slot)&=(uint8_t)~1u; br_sync();
    }
}
static void bb_frame_publish_step(void)
{
    unsigned i=bb.start_offset,end=i+64u;
    if(end>BLE_FRAME_SIZE) end=BLE_FRAME_SIZE;
    /* Build software bytes off-DMA, then store each hardware field at its
     * specified width while stopped. The instance-table entry is still zero. */
    while(i<end) {
        if(i>=100 && i<264) { BB_F8(i)=bb.frame_template[i]; i++; }
        else if((i>=288 && i<312) || i>=320) { BB_F32(i)=ble_get32(bb.frame_template+i); i+=4; }
        else { BB_F16(i)=ble_get16(bb.frame_template+i); i+=2; }
    }
    bb.start_offset=(uint16_t)i;
}
static void bb_payloads(void)
{
    ble_adv_payload(ble_dma.bb+BB_TX0,bb.adv.addr,bb.adv.data,bb.adv.data_len);
    ble_adv_payload(ble_dma.bb+BB_TX1,bb.adv.addr,bb.adv.scan,bb.adv.scan_len);
    BB_F32(BF_POWER)=16u+bb.adv.power; BB_F32(BF_TX_SET)=16u+bb.adv.power;
    br_sync();
}
static void fm1_ble_stop_begin(void);
static void bb_rx_pending(void)
{
    unsigned slot,code,type,n;
    uint8_t *payload;
    if (!(BR_R(0x28028u)&BR_R(0x28030u)&256u)) return;
    BR_R(0x2802cu)=256; br_ssync();
    /* Closing handlers acknowledge but never publish a new payload. */
    if(bb.state==4 || (bb.state==2 && ble_link.state!=BLE_LINK_IDLE)) {
        ble_link.rx_seen=1; /* IRQ timing exists even when both slots wait. */
        bb_connected_rx(fm1_ticks(),0); return;
    }
    if (bb.state!=1) return;
    slot=(BB_F16(BF_RX_TOGGLE)&1u)^1u;
    code=BB_F16(BF_RX_STAT+2*slot)&15u;
    type=BB_F16(BF_RX_ADV+2*slot)&15u;
    n=BB_F16(BF_RX_DATA+2*slot)>>8;
    payload=ble_dma.bb+(slot ? BB_RX1 : BB_RX0);
    if (code!=1) { br_diag.bad_rx++; return; }
    if (type==3 && n==12 && bb.adv.params.type==0 &&
        ((BB_F16(BF_RX_ADV+2*slot)>>7)&1u)==bb.adv.params.own_type &&
        !memcmp(payload+6,bb.adv.addr,6)) {
        unsigned i;
        br_diag.scan_req++;
        for (i=0;i<3;i++) BB_F16(BF_PEER+2*i)=ble_get16(payload+2*i);
    } else if (type==5) {
        ble_health_connect(0);
        ble_conn_t c;
        uint8_t h=(uint8_t)BB_F16(BF_RX_ADV+2*slot);
        if(bb.adv.params.type==0 && ((h>>7)&1u)==bb.adv.params.own_type &&
           !memcmp(payload+6,bb.adv.addr,6) && ble_connect_decode(&c,payload,(uint8_t)n,h)) {
            bb_connection(&c,h,fm1_ticks());
        } else {
            br_diag.connect_ignored++; ble_link.diag.invalid++;
#if BLE_INVALID_CONNECT_RESTART
            bb.restart=1; fm1_ble_stop_begin();
#endif
        }
    } else if (type!=3) br_diag.bad_rx++;
    /* Advertising slots use no full flag; fixed payload buffers are retained.
     * S1 only copies the six-byte scanner address inside this interrupt. */
}
static void fm1_ble_rx_body(void)
{
    bb_mark(BLE_STAGE_RX,BB_F16(BF_EVT));
    br_diag.rx_irqs++;
    bb_rx_pending();
}
static void fm1_ble_event_body(void)
{
    bb_mark(BLE_STAGE_EVENT,BB_F16(BF_EVT));
    uint32_t pending=BR_R(0x28028u)&BR_R(0x28030u),work_start,dt;
    br_diag.event_irqs++;
#if FELUCCA_BLE_STAGE >= 3
    if(bb.state==4) ble_entropy_sample(fm1_ticks(),0,1);
#endif
    /* Equal-level arbitration must not matter: drain RX first. */
    if (pending&256u) {
        uint32_t rx_start=fm1_ticks();
        bb_rx_pending();
        dt=fm1_ticks()-rx_start;
        if (dt>br_diag.rx_max_ticks) br_diag.rx_max_ticks=dt;
        if (dt>380u*24u) br_diag.irq_over_budget++;
    }
    work_start=fm1_ticks(); /* step 1a belongs to RX, not E_ev (02 §7.3a) */
    if (pending&1u) {
        BR_R(0x2802cu)=1;
        if(bb.state==4) {
            uint16_t event=BB_F16(BF_EVT); uint32_t now=fm1_ticks();
            if(ble_link.closed_valid || (int32_t)(now-ble_link.time.first)>=0) {
                ble_ll_event(event,now);
                if(bb.state==4 && ble_link.state<BLE_LINK_CLOSING) {
                    if(ble_link.rx_seen) {
                        if(ble_link.acc>=BLE_MISS_STEP_US) { ble_link.acc=0; bb_window_length(ble_link.window); }
                    } else if(ble_link.widen+ble_link.acc+BLE_MISS_STEP_US<BLE_MISS_LIMIT_US) {
                        uint32_t raw=bb_anchor_read(0)|((bb_anchor_read(14)&255u)<<16);
                        int f=(int)(bb_anchor_read(7)&4095u),next; unsigned drift;
                        if(f&2048) f-=4096;
                        if(ble_missed_step(f,raw,625u-(BR_R(0x2800cu)&1023u)-(BR_R(0x28014u)&255u),&next,&drift)) {
                            bb_anchor(0,16,drift); bb_anchor(0,7,(uint32_t)(int32_t)(int16_t)((uint16_t)next|0x8000u));
                            bb_window_length(ble_link.window+2u*(ble_link.acc+BLE_MISS_STEP_US));
                            ble_link.acc+=BLE_MISS_STEP_US;
                        }
                    }
                }
                ble_link.rx_seen=ble_link.valid_seen=0;
            }
        }
        if (bb.state==1) {
            br_diag.adv_events++;
            if (!br_diag.first_event_ticks) br_diag.first_event_ticks=fm1_ticks();
            if (!bb.restart) {
                bb_payloads(); bb_remap();
                BB_F16(BF_PRIO_CTRL)=26; BB_F16(BF_PRIO_STAT)=26;
            }
        }
    }
    if ((BR_R(0x2804cu)&BR_R(0x28050u))&1u)
        BR_R(0x2804cu)=(BR_R(0x2804cu)&255u)|256u;
    dt=fm1_ticks()-work_start;
    if (dt>br_diag.event_work_max_ticks) br_diag.event_work_max_ticks=dt;
    if (dt>150u*24u) br_diag.event_over_150us++;
    /* Sleep is never entered; do not touch its request/handshake bits. */
}
extern void isr_ble_rx(void),isr_ble_event(void),isr_ble_worker(void);
static void fm1_ble_attach(void)
{
    uint32_t k=fm1_ble_irq_save();
    fm1_irq_attach(29,isr_ble_rx,5); fm1_irq_mask(29);
    fm1_irq_attach(45,isr_ble_event,5); fm1_irq_mask(45);
    fm1_irq_attach(4,isr_ble_worker,2); fm1_irq_mask(4);
    fm1_ble_irq_restore(k);

}
static void fm1_ble_startup(ble_cal_t *cal)
{
    unsigned i;
    if(bb.state || (ble_link.state!=BLE_LINK_IDLE && ble_link.state!=BLE_LINK_STOPPED)) { br_error(10); return; }
    memset(&ble_dma,0,sizeof ble_dma);
    for (i=0;i<4;i++) ble_dma.canary[i]=0x4252444du;
    br_diag.dma_base=(uint32_t)(uintptr_t)ble_dma.bb;
    br_diag.dma_end=br_diag.dma_base+BLE_BB_SIZE;
    br_diag.modem_base=(uint32_t)(uintptr_t)ble_dma.modem;
    br_radio_init(cal,ble_dma.modem);
    br_diag.bringup_end_ticks=fm1_ticks();
    if (br_diag.error) return;
    bb_zero_anchors();
    BR_R(0x2fc80u)|=1; BR_R(0x28000u)=1; BR_R(0x28034u)=0xa01;
    BR_R(0x2fc84u)=(uint32_t)(uintptr_t)ble_dma.bb;
    BR_R(0x2fcbcu)=(uint32_t)(uintptr_t)(ble_dma.bb+BLE_BB_SIZE);
    br_field(0x28000u,0x6010,0x6000);
    br_field(0x28008u,0xf3ff,0xf046); br_field(0x2800cu,1023,70);
    br_field(0x28010u,65535,0x3e3e); br_field(0x28014u,255,30); br_field(0x28018u,65535,0x1414);
}
static int fm1_ble_adv_start(const ble_adv_t *adv)
{
    uint32_t k;
    if (br_diag.error || bb.state) return -1;
    if (BR_R(0x28038u)&2u) { br_error(6); return -1; }
    /* Closed instance: no DMA owner or enabled peripheral source. Prepare
     * the large window outside the short publication critical section. */
    if(!bb.start_phase) {
        bb.adv=*adv; bb.start_offset=0; bb.start_phase=1;
        bb.tx_id[0]=bb.tx_id[1]=-1; bb.rx_delayed[0]=bb.rx_delayed[1]=0;
    }
    if(bb.start_phase==1) {
        unsigned n=sizeof ble_dma.bb-bb.start_offset;
        if(n>256) n=256;
        memset(ble_dma.bb+bb.start_offset,0,n); bb.start_offset=(uint16_t)(bb.start_offset+n);
        if(bb.start_offset==sizeof ble_dma.bb) bb.start_phase=2;
        return 1;
    }
    if(bb.start_phase==2) {
        ble_frame_init(bb.frame_template,&bb.adv); bb.start_offset=0; bb.start_phase=3; return 1;
    }
    if(bb.start_phase==3) {
        bb_frame_publish_step(); if(bb.start_offset==BLE_FRAME_SIZE) bb.start_phase=4;
        return 1;
    }
    if(bb.start_phase==4) {
        bb_payloads(); BB_F16(BF_RX_PTR)=BB_RX0; BB_F16(BF_RX_PTR+2)=BB_RX1;
        BB_F8(BF_RX_CTL)&=(uint8_t)~1u; BB_F8(BF_RX_CTL+1)&=(uint8_t)~1u;
        bb_remap(); bb.start_phase=5; return 1;
    }
    k=fm1_ble_irq_save();
    /* P6: attach left these CPU lines masked throughout P2/P3/P4. */
    fm1_ble_line_enable(29); fm1_ble_line_enable(45);
    br_sync(); *(volatile uint16_t *)(void *)ble_dma.bb=BB_FRAME|0x8000u; br_sync();
    bb_anchor(0,1,adv->params.max); bb_anchor(0,15,0x8000);
    bb_anchor(0,2,0x2000); bb_anchor(0,8,0xc000);
    bb_anchor(0,6,ble_adv_channel_word(adv->params.channels));
    BR_R(0x2802cu)=257;
    BR_R(0x2804cu)=(BR_R(0x2804cu)&255u)|256u;
    BR_R(0x28028u)|=257; BR_R(0x2804cu)=(BR_R(0x2804cu)&255u)|1u;
    /* RX acceptance state precedes the first hardware enable. Both TX
     * descriptors, identity, data and CONNECT_IND decoder are ready now. */
    bb.state=1; ble_health_adv_begin(fm1_ticks());
    bb_anchor(0,7,0); bb_anchor(0,0,0); bb_anchor(0,14,0);
    bb_anchor(0,0,0);
    br_diag.enable_ticks=fm1_ticks(); br_diag.first_event_ticks=0;
    bb_anchor(0,14,0x8000); BB_F16(BF_MODE)|=4;
    br_sync(); fm1_ble_irq_restore(k);
    bb.start_phase=0;
    return 0;
}
static void fm1_ble_adv_update(const ble_adv_t *adv)
{
    uint32_t k=fm1_ble_irq_save();
    bb.adv=*adv; /* shadow only; payload and power stores at event close */
    fm1_ble_irq_restore(k);
}
static void fm1_ble_stop_begin(void)
{
    uint32_t k=fm1_ble_irq_save();
    if (bb.state!=1 && bb.state!=4) { fm1_ble_irq_restore(k); return; }
    ble_health_begin(BLE_GAP_RADIO_STOP);
    bb.state=2; /* closing and refill exclusion under the same lock */
    bb_anchor(0,14,0); bb.stop_start=fm1_ticks();
    bb.stop_bound=ble_link.state!=BLE_LINK_IDLE ? (uint32_t)ble_link.conn.interval*2500u : (uint32_t)bb.adv.params.max*1250u;
    if (bb.stop_bound<20000u) bb.stop_bound=20000;
    if (bb.stop_bound>100000u) bb.stop_bound=100000;
    bb.stop_bound*=24;
    fm1_ble_irq_restore(k);
}
static int fm1_ble_stop_poll(void)
{
    uint32_t k;
    unsigned i;
    if (bb.state==3) return -1;
    if (!bb.state) return 1;
    if (bb.state!=2) return 0;
    if (BR_R(0x28038u)&2u) {
        if ((uint32_t)(fm1_ticks()-bb.stop_start)<bb.stop_bound) return 0;
        /* Quarantine retains the frame, window and every buffer. No abort. */
        ble_health_end(BLE_GAP_RADIO_STOP);
        bb.state=3; br_diag.stop_timeouts++; br_error(7); return -1;
    }
    k=fm1_ble_irq_save();
    bb_refill(); /* settle control-byte acknowledgements before DMA reclaim */
    BR_R(0x28028u)&=~257u;
    BR_R(0x2804cu)=(BR_R(0x2804cu)&254u)|256u; BR_R(0x2802cu)=257;
    for (i=0;i<17;i++) bb_anchor(0,i,0);
    *(volatile uint16_t *)(void *)ble_dma.bb=0; br_sync();
    fm1_ble_irq_restore(k);
    /* Reclaim only after busy cleared and the source was masked/ACKed.
     * Zero the active descriptor only: do not burn an IRQ budget on 3.3 KB. */
    memset(ble_dma.bb+BB_FRAME,0,BLE_FRAME_SIZE);
    k=fm1_ble_irq_save(); bb.state=0; ble_health_end(BLE_GAP_RADIO_STOP); fm1_ble_irq_restore(k);
    return 1;
}
static void fm1_ble_service(void)
{
    uint32_t t=fm1_ticks(),gap=t-bb.last_worker;
    if (bb.last_worker && ble_link.state!=BLE_LINK_IDLE && gap>br_diag.worker_gap_ticks) br_diag.worker_gap_ticks=gap;
    bb.last_worker=t;
    if (bb.restart && bb.state!=3) {
        if (bb.state==1) fm1_ble_stop_begin();
        if (fm1_ble_stop_poll()==1) {
            ble_adv_t adv=bb.adv;
            int rc=fm1_ble_adv_start(&adv);
            if(rc<=0) { bb.restart=0; br_diag.restarts++; }
            else ble_port_pend();
        }
    }
    if ((uint32_t)(t-bb.last_canary)>=24000000u) {
        unsigned i;
        bb.last_canary=t;
        for (i=0;i<4;i++) if (ble_dma.canary[i]!=0x4252444du) {
            br_diag.canary_errors++; br_error(8);
            if (bb.state==1) fm1_ble_stop_begin();
        }
    }
}
static int fm1_ble_flash_read(uint32_t off,uint8_t *out,uint32_t n)
{
    uint32_t i;
    /* Plain XIP mapping was installed by fm1-base boot. No raw read, IRQ-off
     * flash window, item write, or pointer outside the 1 MiB flash. */
    if (!((off>=0x93000u && off<=0x94000u && n<=0x94000u-off) ||
          (off>=0xe9000u && off<=0xea000u && n<=0xea000u-off))) return -1;
    for (i=0;i<n;i++) out[i]=*(const volatile uint8_t *)(uintptr_t)FL_XIP(off+i);
    return 0;
}
static inline void fm1_ble_readback(uint32_t *out)
{
    unsigned i;
    for (i=0;i<31;i++) out[i]=BR_A(i);
    for (i=0;i<8;i++) out[31+i]=BR_R(0x14000u+4*i);
    for (i=0;i<10;i++) out[39+i]=BR_R(0x14040u+4*i);
    out[49]=BR_R(0x20000u);
}
/* r4 timer alternative: software requests a 1-us expiry on dedicated
 * TIMER0 / IRQ4, level 2. A 1-ms autonomous expiry is the backstop. */
static volatile uint8_t ble_llx_enabled,ble_llx_pending,ble_llx_running,ble_llx_testing;
static uint32_t ble_llx_request_ticks;
static void fm1_ble_worker_timer_arm(uint32_t count)
{
    BR_R(0x10400u)=0x4000u; BR_R(0x10404u)=0; BR_R(0x10408u)=count;
    BR_R(0x10400u)=0x4019u;
}
static void fm1_ble_worker_pend(void)
{
    uint32_t key=fm1_ble_irq_save();
    if(ble_llx_enabled && !ble_llx_pending) {
        ble_llx_pending=1; ble_llx_request_ticks=fm1_ticks();
        if(!ble_llx_running) fm1_ble_worker_timer_arm(6u);
    }
    fm1_ble_irq_restore(key);
}
static void fm1_ble_worker_timer_start(void)
{
    uint32_t before=ble_health.calls,start=fm1_ticks();
    ble_llx_testing=ble_llx_enabled=1; fm1_ble_line_enable(4); fm1_ble_worker_pend();
    while(*(volatile uint32_t *)&ble_health.calls==before && (uint32_t)(fm1_ticks()-start)<24000u) ;
    ble_llx_testing=0;
    if(*(volatile uint32_t *)&ble_health.calls!=before+1u) {
        ble_llx_enabled=0; fm1_irq_mask(4); br_error(10); return;
    }
    br_diag.executor_selftest=1; fm1_ble_worker_timer_arm(6000u);
}
static void fm1_ble_worker_timer_ack(void) { BR_R(0x10400u)=0x4000u; }
static inline void fm1_ble_worker_timer_readback(uint32_t out[4])
{
    out[0]=BR_R(0x10400u); out[1]=BR_R(0x10404u); out[2]=BR_R(0x10408u);
    out[3]=(FM1_ICFG(4)>>16)&15u;
}
static void bb_window(uint32_t us) { bb_window_length(us); BB_F16(BF_WIN_STEADY)=BLE_WINDOW_PAD_US; }
static void bb_priority(unsigned p) { BB_F16(BF_PRIO_CTRL)=(uint16_t)p; BB_F16(BF_PRIO_STAT)=(uint16_t)p; }
static void bb_latency(uint16_t latency,int enabled)
{
    bb_anchor(0,2,0x7000u|latency|((enabled && latency) ? 0x800u : 0u));
    bb_anchor(0,9,64u);
}
static void bb_apply(uint8_t kind,const ble_conn_t *c,uint16_t widening,uint32_t window)
{
    if(kind==2) { bb_map(c->map); return; }
    bb_window(window); bb_latency(c->latency,0);
    BB_F16(72)=widening/625u; BB_F16(74)=(uint16_t)(0x5000u|(widening%625u));
    bb_anchor(0,4,c->offset ? (2u*c->offset-1u)|0x8000u : 0u);
    bb_anchor(0,1,c->interval*2u); bb_anchor(0,15,0x8000u);
}
static int fm1_ble_store_map_valid(void)
{
    static const uint32_t map[OBJ_NOBJ]=APP_STORE_MAP;
    unsigned i,j;
    if(OBJ_BLE>=OBJ_NOBJ) return 0;
    for(i=0;i<OBJ_NOBJ;i++) {
        if((map[i]&4095u) || !FL_STORE_OK(map[i],8192u) || !FL_CAL_CLEAR(map[i],8192u)) return 0;
        for(j=0;j<i;j++) if(map[i]<map[j]+8192u && map[j]<map[i]+8192u) return 0;
    }
    return 1;
}
static int fm1_ble_link_active(void) { return ble_link.state!=BLE_LINK_IDLE; }
#undef BB_F16
#undef BB_F8
#undef BB_F32

#pragma clang diagnostic pop
