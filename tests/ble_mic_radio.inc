/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Included by ble_s2_sim.c only for the MIC suite. The two functions in
 * hal_mic_seam.inc are extracted unchanged from the production HAL. */
#include <assert.h>
#include "ble_mic_radio.h"
#include "../open_ble/ble_seed.h"
#include "../open_ble/ble_security.h"
static struct { _Alignas(4) uint8_t bb[BLE_BB_SIZE]; } ble_dma;
static struct {
    uint8_t state,refill_busy,rx_delayed[2];
#if FM1B_BLE_RELIABILITY
    uint8_t rx_pinned[2];
#endif
    int8_t tx_id[2];
    uint32_t rx_stamp[2];
} bb;
enum { BB_FRAME=16,BB_RX0=340,BB_RX1=596,BB_TX0=852,BB_TX1=892 };
#define BB_F16(o) (*(uint16_t *)(void *)(ble_dma.bb+BB_FRAME+(o)))
#define BB_F8(o) (*(uint8_t *)(void *)(ble_dma.bb+BB_FRAME+(o)))
#define BB_F32(o) (*(uint32_t *)(void *)(ble_dma.bb+BB_FRAME+(o)))
#if FM1B_BLE_RELIABILITY
int ble_port_diag_radio_idle(void) { return 1; }
void sim_radio_flip_tx_toggle(void) { BB_F16(BF_TX_TOGGLE)^=1u; }
void sim_radio_corrupt(unsigned slot,unsigned byte)
{
    assert(slot<2 && byte<31); ble_dma.bb[(slot ? BB_RX1 : BB_RX0)+byte]^=1;
}
#endif
void (*sim_radio_release_hook)(void);
uint32_t sim_radio_pressure,sim_radio_both,sim_radio_refused;
static uint32_t fm1_ticks(void) { return sim_ticks; }
static uint32_t fm1_ble_irq_save(void) { return ble_port_lock(); }
static void fm1_ble_irq_restore(uint32_t key) { ble_port_unlock(key); }
static void br_sync(void)
{
    if(sim_radio_release_hook && (!BB_F8(BF_RX_CTL) || !BB_F8(BF_RX_CTL+1))) {
        void (*hook)(void)=sim_radio_release_hook; sim_radio_release_hook=0; hook();
    }
}
#include "hal_mic_seam.inc"
static void sim_radio_mirror(void)
{
    for(unsigned i=0;i<2;i++) {
        sim_slot[i]=bb.tx_id[i]; sim_air_len[i]=(uint8_t)(BB_F16(BF_TX_DATA+2*i)>>8);
        memcpy(sim_air[i],ble_dma.bb+(i ? BB_TX1 : BB_TX0),sim_air_len[i]);
    }
}
void ble_port_refill(void) { bb_refill(); sim_radio_mirror(); }
void ble_port_refill_one(void) { bb_refill_limit(1); sim_radio_mirror(); }
void ble_port_rx_recover(void) { if(bb.state==4) { bb_connected_rx(sim_ticks,1); sim_radio_mirror(); } }
static void sim_radio_reset(void)
{
    memset(&bb,0,sizeof bb); memset(&ble_dma,0,sizeof ble_dma);
    bb.tx_id[0]=bb.tx_id[1]=-1; sim_radio_release_hook=0;
}
void sim_radio_start(void)
{
    bb.state=4;
    for(unsigned i=0;i<2;i++) {
        bb.rx_stamp[i]=sim_ticks; BB_F8(BF_TX_CTL+i)=1;
        BB_F16(BF_TX_DATA+2*i)=(uint16_t)(1u|(i ? 4u : 0));
    }
}
unsigned sim_radio_full(void) { return (BB_F8(BF_RX_CTL)&1u)+ (BB_F8(BF_RX_CTL+1)&1u); }
int sim_radio_receive(uint16_t status,uint16_t header,const uint8_t *data,uint16_t event)
{
    unsigned slot=BB_F16(BF_RX_TOGGLE)&1u,n=header>>8;
    BB_F16(BF_EVT)=event;
    if(BB_F8(BF_RX_CTL+slot)&1u) { sim_radio_refused++; return 0; }
    assert(n<=255);
    if(n) memcpy(ble_dma.bb+(slot ? BB_RX1 : BB_RX0),data,n);
    BB_F16(BF_RX_STAT+2*slot)=status; BB_F16(BF_RX_DATA+2*slot)=header;
    BB_F8(BF_RX_CTL+slot)=1; BB_F16(BF_RX_TOGGLE)^=1u;
    if(sim_radio_full()==2) sim_radio_both++;
    return 1;
}
void sim_radio_service(int recovery)
{
    bb_connected_rx(sim_ticks,recovery); sim_radio_mirror();
    if(bb.rx_delayed[0] || bb.rx_delayed[1]) sim_radio_pressure++;
}
static void sim_radio_ack(unsigned slot)
{
    BB_F8(BF_TX_CTL+slot)=1;
    /* TX toggle and the slot SN pattern belong to the virtual baseband. */
    BB_F16(BF_TX_TOGGLE)=(uint16_t)(slot^1u);
    bb_refill(); sim_radio_mirror(); /* boundaries remain pending until RX */
}
/* Independent from the central: two one-bit ARQ machines and an immutable
 * last transmitted PDU, including automatic empty packets. The documented
 * register bus does not specify how a newly loaded slot interlocks with a
 * hardware-generated empty PDU that is still awaiting acknowledgment. */
static struct {
    uint8_t rx_sn,tx_sn,valid,loaded,slot,length,llid,md,data[31];
} core_air;
uint32_t sim_core_empty_pending_loads;
uint8_t sim_core_start(uint8_t rx_sn)
{
    memset(&core_air,0,sizeof core_air);
    core_air.rx_sn=rx_sn; core_air.tx_sn=(uint8_t)(BB_F16(BF_TX_TOGGLE)&1u);
    sim_core_empty_pending_loads=0; return core_air.tx_sn;
}
int sim_core_receive(uint16_t header,const uint8_t *data,uint16_t event,int crc)
{
    unsigned fresh=((header>>3)&1u)==core_air.rx_sn;
    if(crc && core_air.valid && ((header>>2)&1u)!=core_air.tx_sn) {
        if(core_air.loaded) BB_F8(BF_TX_CTL+core_air.slot)=1;
        else if(!(BB_F8(BF_TX_CTL+core_air.slot)&1u)) sim_core_empty_pending_loads++;
        core_air.tx_sn^=1u; BB_F16(BF_TX_TOGGLE)^=1u; core_air.valid=0;
    }
    unsigned cls=crc ? (fresh ? 1u : 9u) : (fresh ? 5u : 13u);
    int stored=sim_radio_receive((uint16_t)cls,header,data,event);
    if(stored && crc && fresh) core_air.rx_sn^=1u;
    return stored && crc && fresh;
}
void sim_core_transmit(uint16_t *header,uint8_t data[31])
{
    if(!core_air.valid) {
        unsigned slot=BB_F16(BF_TX_TOGGLE)&1u;
        core_air.slot=(uint8_t)slot; core_air.loaded=!(BB_F8(BF_TX_CTL+slot)&1u);
        core_air.length=core_air.loaded ? (uint8_t)(BB_F16(BF_TX_DATA+2*slot)>>8) : 0;
        core_air.llid=core_air.loaded ? (uint8_t)(BB_F16(BF_TX_DATA+2*slot)&3u) : 1;
        core_air.md=core_air.loaded ? (uint8_t)((BB_F16(BF_TX_DATA+2*slot)>>3)&1u) : 0;
        if(core_air.length) memcpy(core_air.data,ble_dma.bb+(slot ? BB_TX1 : BB_TX0),core_air.length);
        core_air.valid=1;
    }
    *header=(uint16_t)((core_air.length<<8)|core_air.llid|(core_air.tx_sn<<3)|
                      (core_air.rx_sn<<2)|(core_air.md<<4));
    if(core_air.length) memcpy(data,core_air.data,core_air.length);
}
