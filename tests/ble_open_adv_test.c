/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
/* Actual HAL functions, extracted without changing their bodies. Only the
 * register bus, IRQ gate, clock, anchors and LL create are simulated. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../open_ble/ble_link.h"
#include "../open_ble/ble_health.h"
#include "../open_ble/ble_breadcrumb.h"
static uint32_t registers[0x40000/4],now=24000;
#define BR_R(a) registers[(a)/4u]
static ble_hw_diag_t br_diag;
struct ble_link ble_link;
static struct { uint8_t bb[BLE_BB_SIZE]; } ble_dma;
static struct {
    ble_adv_t adv;
    uint8_t frame_template[BLE_FRAME_SIZE];
    uint32_t last_worker;
    uint16_t start_offset;
    uint8_t start_phase,state,refill_busy,rx_delayed[2];
    int8_t tx_id[2];
    uint32_t rx_stamp[2];
} bb;
enum { BB_FRAME=16,BB_RX0=340,BB_RX1=596,BB_TX0=852,BB_TX1=892 };
#define BB_F16(o) (*(uint16_t *)(void *)(ble_dma.bb+BB_FRAME+(o)))
#define BB_F8(o) (*(uint8_t *)(void *)(ble_dma.bb+BB_FRAME+(o)))
#define BB_F32(o) (*(uint32_t *)(void *)(ble_dma.bb+BB_FRAME+(o)))
static void br_sync(void) {}
static void br_ssync(void) {}
static void br_error(uint32_t e) { br_diag.error=e; }
static uint32_t fm1_ticks(void) { return now; }
static void fm1_ble_line_enable(unsigned n) { (void)n; }
static uint32_t fm1_ble_irq_save(void) { return 0; }
static void bb_mark(uint8_t stage,uint16_t d) { (void)stage; (void)d; }
static void bb_window_length(uint32_t us) { BB_F16(BF_WIN)=(uint16_t)us; }
static void bb_anchor(unsigned id,unsigned index,uint32_t value) { (void)id; (void)index; (void)value; }
static void bb_connected_rx(uint32_t t,int recovery) { (void)t; (void)recovery; }
int ble_ll_create(const ble_conn_t *c,uint32_t t)
{
    (void)t;
    if(ble_link.state!=BLE_LINK_IDLE) return 0;
    ble_link.conn=*c; ble_link.epoch++; ble_link.state=BLE_LINK_CREATED; return 1;
}
static unsigned timing_resets;
void ble_time_reset(void) { timing_resets++; }
static int accept_at_unlock;
static void bb_rx_pending(void);
static void fm1_ble_irq_restore(uint32_t key)
{
    (void)key;
    if(accept_at_unlock && (BB_F16(BF_MODE)&4u)) {
        accept_at_unlock=0;
        assert(bb.state==1 && !bb.adv.params.type);
        assert(BB_F16(BF_TX_PTR)==BB_TX0 && BB_F16(BF_TX_PTR+2)==BB_TX1);
        assert(BB_F16(BF_TX_ADV)==0 && BB_F16(BF_TX_ADV+2)==4); /* ADV_IND / SCAN_RSP */
        assert(BB_F16(BF_TX_DATA)==(bb.adv.data_len+6u)*256u);
        assert(!memcmp(ble_dma.bb+BB_TX0,bb.adv.addr,6));
        assert(!memcmp(ble_dma.bb+BB_TX0+6,bb.adv.data,bb.adv.data_len));
        bb_rx_pending(); /* First possible RX, before adv_start returns. */
    }
}
#include "hal_adv_seam.inc"
int main(void)
{
    ble_adv_t adv={0};
    adv.params.min=adv.params.max=32; adv.params.channels=7; adv.power=6;
    memcpy(adv.addr,(uint8_t[]){1,2,3,4,5,6},6);
    memcpy(adv.data,(uint8_t[]){2,1,6},3); adv.data_len=3;
    ble_health_init(now,1);
    for(unsigned session=1;session<=6;session++) {
        int rc=1;
        bb.state=0; ble_link.state=BLE_LINK_IDLE;
        memset(registers,0,sizeof registers);
        for(unsigned steps=0;rc>0;steps++) {
            assert(steps<30 && !ble_health.adv.seen);
            if(bb.start_phase==5) {
                uint8_t *p=ble_dma.bb+BB_RX1;
                p[0]=7; memcpy(p+6,adv.addr,6);
                ble_put32(p+12,0x12345678); p[16]=0x56; p[17]=0x34; p[18]=0x12;
                p[19]=1; ble_put16(p+22,12); ble_put16(p+26,600);
                memset(p+28,255,4); p[32]=31; p[33]=5;
                BB_F16(BF_RX_STAT+2)=1; BB_F16(BF_RX_ADV+2)=5; BB_F16(BF_RX_DATA+2)=34u<<8;
                BR_R(0x28030u)=256; accept_at_unlock=1;
            }
            rc=fm1_ble_adv_start(&adv);
            if(rc>0) assert(!bb.state && !(BB_F16(BF_MODE)&4u));
        }
        assert(!rc && bb.state==4 && ble_link.epoch==session);
        assert(ble_health.adv.session==session && ble_health.adv.seen==1 && ble_health.adv.accepted==1);
        assert(!memcmp(&bb.adv,&adv,sizeof adv));
        /* Reject malformed CONNECT_IND without accepting or changing session. */
        bb.state=1; ble_link.state=BLE_LINK_IDLE;
        BB_F16(BF_RX_DATA+2)=33u<<8; bb_rx_pending();
        assert(ble_health.adv.seen==2 && ble_health.adv.accepted==1);
        ble_health.adv.seen=0; /* next session asserts no premature RX */
    }
    ble_health_sample(now,BLE_HEALTH_ADVERTISING,6);
    now+=17804u*24u; ble_health_sample(now,BLE_HEALTH_OFF,6);
    ble_health_t before_reset=ble_health;
    ble_port_reset_stats();
    assert(timing_resets==1 && !memcmp(&before_reset,&ble_health,sizeof ble_health));
    puts("HAL restart: closed while preparing; first ADV_IND accepts CONNECT_IND before start returns; six sessions stable; invalid counted: PASS");
}
