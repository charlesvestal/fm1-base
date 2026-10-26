/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* fm1-base R7. Included only with FM1B_BLE_RELIABILITY. Frozen records and
 * counters survive HCI Reset. Private keys never enter the public records. */
#include "ble_reliability_ccm.inc"
static struct {
    ble_reliability_stats_t stats;
    ble_reliability_record_t rx,tx;
    struct {
        uint8_t ltk[16],skd_input[16],derived[16];
        uint8_t clear[10],skd[16],iv[8];
        uint32_t enc_ms,ack_ms,connect_gap,connect_close_gap,old_create,old_enc;
        uint8_t inputs,derived_valid,rx_ok,tx_ok,enc_valid,ack_valid;
    } link;
    uint8_t previous_key[16],previous_iv[8],previous_valid;
    uint32_t disconnect_ms,disconnect_complete_ms,last_kat_ms;
    uint8_t disconnect_valid,disconnect_complete_valid;
    struct {
        uint8_t active,ready,id;
        uint32_t epoch,started;
        uint64_t counter;
        uint8_t expected[31];
    } pending;
    uint8_t *fault_dma;
    ble_reliability_record_t *fault;
    uint8_t failure_active,phase,trial,sw_active,closing_diag;
    uint8_t tx_sw_active;
} reliability __attribute__((section(".pool.ble")));
static uint32_t reliability_old_work(void)
{
    uint32_t mask=ble_reliability_old_commands(0);
    for(unsigned i=0;i<BLE_TX_COUNT;i++)
        if(queues.tx[i].state && queues.tx[i].epoch!=ble_link.epoch) mask|=1u;
    for(unsigned i=0;i<queues.count;i++)
        if(queues.rx[(queues.r+i)%BLE_RX_STREAM].epoch!=ble_link.epoch) mask|=2u;
    if(queues.control_ready && queues.control.epoch!=ble_link.epoch) mask|=4u;
    ble_aes_state_t a; ble_port_aes_state(&a);
    if(a.busy && a.epoch!=ble_link.epoch) mask|=8u;
    if(reliability.pending.active && reliability.pending.epoch!=ble_link.epoch) mask|=16u;
    return mask;
}
static uint32_t reliability_work_before_create(void)
{
    uint32_t mask=reliability_old_work()|ble_reliability_old_commands(1);
    for(unsigned i=0;i<BLE_TX_COUNT;i++) if(queues.tx[i].state) mask|=1u;
    if(queues.count) mask|=2u;
    if(queues.control_ready) mask|=4u;
    if(queues.refill_pending) mask|=32u;
    if(security.reply_pending || security.random_pending || security.ltk_event) mask|=64u;
    ble_aes_state_t a; ble_port_aes_state(&a); if(a.busy) mask|=8u;
    if(ble_reliability_pending()) mask|=16u;
    return mask;
}
static void reliability_created(uint32_t old)
{
    reliability.link.old_create=old;
    reliability.link.connect_gap=reliability.disconnect_complete_valid ? ble_port_ms()-reliability.disconnect_complete_ms : UINT32_MAX;
    reliability.link.connect_close_gap=reliability.disconnect_valid ? ble_port_ms()-reliability.disconnect_ms : UINT32_MAX;
}
static void reliability_disconnected(void)
{
    reliability.disconnect_complete_ms=ble_port_ms(); reliability.disconnect_complete_valid=1;
}
static void reliability_ack(void)
{
    reliability.link.ack_ms=ble_port_ms(); reliability.link.ack_valid=1;
}
static void reliability_cancel(void)
{
    ble_secure_zero(&reliability.link,sizeof reliability.link);
    ble_secure_zero(&reliability.pending,sizeof reliability.pending);
    if(reliability.tx_sw_active || reliability.sw_active || reliability.failure_active)
        ble_secure_zero(&verify,sizeof verify);
    reliability.tx_sw_active=reliability.sw_active=0;
}
static void reliability_disconnect(void)
{
    /* Most recent link only, including an unencrypted link (then unavailable).
     * Refresh within a link does not replace the previous-link comparator. */
    ble_secure_zero(reliability.previous_key,16); ble_secure_zero(reliability.previous_iv,8);
    reliability.previous_valid=reliability.link.derived_valid;
    if(reliability.previous_valid) {
        memcpy(reliability.previous_key,security.key,16);
        memcpy(reliability.previous_iv,security.iv,8);
    }
    reliability.disconnect_ms=ble_port_ms(); reliability.disconnect_valid=1; reliability.disconnect_complete_valid=0;
    reliability_cancel();
}
static int reliability_reset_cleanup(void)
{
    if(reliability.link.derived_valid) reliability_disconnect(); else reliability_cancel();
    return 1;
}
static void reliability_inputs_wipe(void)
{
    if(reliability.link.rx_ok && reliability.link.tx_ok) {
        ble_secure_zero(reliability.link.ltk,16); ble_secure_zero(reliability.link.skd_input,16);
        reliability.link.inputs=0;
    }
}
static void reliability_context(ble_reliability_record_t *d,uint8_t kind,uint64_t counter)
{
    memset(d,0,sizeof *d); d->kind=kind; d->counter=counter; d->epoch=ble_link.epoch;
    d->ms=ble_port_ms(); d->connect_gap_ms=reliability.link.connect_gap;
    d->connect_close_gap_ms=reliability.link.connect_close_gap;
    d->previous_disconnect_ms=reliability.disconnect_complete_valid ? d->ms-reliability.disconnect_complete_ms : UINT32_MAX;
    d->previous_close_ms=reliability.disconnect_valid ? d->ms-reliability.disconnect_ms : UINT32_MAX;
    d->enc_req_age_ms=reliability.link.enc_valid ? d->ms-reliability.link.enc_ms : UINT32_MAX;
    d->start_ack_age_ms=reliability.link.ack_valid ? d->ms-reliability.link.ack_ms : UINT32_MAX;
    d->old_work_create=reliability.link.old_create; d->old_work_enc=reliability.link.old_enc;
    d->event=ble_link.diag.event; d->slot=255;
    d->derived_valid=reliability.link.derived_valid; d->inputs_retained=reliability.link.inputs;
    d->previous_valid=reliability.previous_valid;
    d->key_unchanged=d->derived_valid ? !memcmp(security.key,reliability.link.derived,16) : 2;
    d->iv_unchanged=d->derived_valid ? !memcmp(security.iv,reliability.link.iv,8) : 2;
    d->hw_retry=d->sw_same=d->key_recomputed=d->previous_key=d->previous_context=d->kat=2;
    d->dma_equal=d->dma_input_equal[0]=d->dma_input_equal[1]=2;
    memcpy(d->rand_ediv,reliability.link.clear,10); memcpy(d->skd,reliability.link.skd,16);
    memcpy(d->iv,reliability.link.iv,8); memcpy(d->iv_used,security.iv,8);
}
static void reliability_dma(ble_reliability_record_t *d,unsigned which)
{
    if(d->kind==1) d->dma_valid[which]=(uint8_t)ble_port_diag_rx_read(d->slot,
        &d->dma_header[which],&d->dma_status[which],d->dma[which]);
    else if(d->kind==2) {
        /* TX remains unpublished and CPU-owned until this verifier finishes. */
        volatile const uint8_t *p=reliability.fault_dma;
        for(unsigned i=0;i<d->length;i++) d->dma[which][i]=p[i];
        d->dma_valid[which]=1; d->dma_header[which]=d->header;
    }
    if(d->dma_valid[which]) d->dma_input_equal[which]=(uint8_t)(d->header==d->dma_header[which] &&
        !memcmp(d->raw,d->dma[which],d->length));
    if(which && d->dma_valid[0] && d->dma_valid[1]) d->dma_equal=(uint8_t)(
        d->dma_header[0]==d->dma_header[1] && d->dma_status[0]==d->dma_status[1] &&
        !memcmp(d->dma[0],d->dma[1],d->length));
}
static void reliability_rx_failure(const rx_t *r,uint64_t count,const uint8_t *raw,const ble_aes_state_t *before)
{
    if(reliability.rx.kind || reliability.failure_active) return;
    ble_reliability_record_t *d=&reliability.rx;
    reliability_context(d,1,count); d->header=r->header; d->status=r->status; d->event=r->event;
    d->slot=r->slot; d->length=r->n; d->direction=1;
    memcpy(d->raw_before,raw,r->n); memcpy(d->raw,r->data,r->n);
    d->input_stable=(uint8_t)!memcmp(raw,r->data,r->n); d->before=*before; ble_port_aes_state(&d->failed);
    reliability_dma(d,0); reliability.fault=d; reliability.failure_active=1;
    reliability.phase=reliability.trial=reliability.sw_active=0;
    ble_radio_freeze(0x3d);
    /* No TX software job may overwrite the verifier during failure analysis. */
    reliability.tx_sw_active=0; ble_secure_zero(&verify,sizeof verify); ble_port_pend();
}
static int reliability_hold_close(uint8_t reason)
{
    if(reliability.closing_diag) return 0;
    if(reliability.failure_active) return 1;
    if(reason!=0x3d || reliability.rx.kind) return 0;
    /* A local phase/boundary 0x3D without a rejected ciphertext is explicit,
     * never mislabeled as a CCM failure. Still collect key checks and KAT. */
    reliability_context(&reliability.rx,3,security.d.rx_counter);
    reliability.fault=&reliability.rx; reliability.failure_active=1;
    reliability.phase=reliability.trial=reliability.sw_active=0;
    ble_radio_freeze(0x3d);
    ble_port_aes_state(&reliability.rx.before); reliability.rx.failed=reliability.rx.before;
    reliability.tx_sw_active=0; ble_secure_zero(&verify,sizeof verify); ble_port_pend(); return 1;
}
int ble_reliability_failed(void) { return reliability.failure_active; }
int ble_reliability_pending(void)
{
    return reliability.failure_active || reliability.pending.active;
}
static int reliability_kat(void)
{
    /* Core sample, the same independent fixed answer used at boot. */
    static const uint8_t key[16]={0xbf,1,0xfb,0x9d,0x4e,0xf3,0xbc,0x36,0xd8,0x74,0xf5,0x39,0x41,0x38,0x68,0x4c};
    static const uint8_t text[16]={0x13,2,0xf1,0xe0,0xdf,0xce,0xbd,0xac,0x79,0x68,0x57,0x46,0x35,0x24,0x13,2};
    static const uint8_t answer[16]={0x66,0xc6,0xc2,0x27,0x8e,0x3b,0x8e,5,0x3e,0x7e,0xa3,0x26,0x52,0x1b,0xad,0x99};
    uint8_t out[16]; int ok=ble_aes_le(key,text,out) && !memcmp(out,answer,16);
    ble_secure_zero(out,16); reliability.stats.kat_runs++; reliability.stats.kat_failures+=!ok;
    return ok;
}
static void reliability_failure_step(void)
{
    ble_reliability_record_t *d=reliability.fault;
    switch(reliability.phase) {
    case 0: {
        uint8_t p[31]; memcpy(p,d->raw,d->length);
        if(d->length>=5 && d->counter<=BLE_CCM_LAST)
            d->hw_retry=(uint8_t)ble_ccm(security.key,security.iv,d->counter,d->direction,
                (uint8_t)(d->header&3u),p,(uint8_t)(d->length-4u),1);
        ble_secure_zero(p,sizeof p); ble_port_aes_state(&d->after_retry); reliability.phase++; return;
    }
    case 1: d->kat=(uint8_t)reliability_kat(); reliability.phase++; return;
    case 2: /* Software session-key derivation, separated expansion/block. */
        if(!reliability.link.inputs) { reliability.phase=4; return; }
        for(unsigned i=0;i<16;i++) verify.key[i]=reliability.link.ltk[15u-i];
        tc_aes128_set_encrypt_key(&verify.aes,verify.key); ble_secure_zero(verify.key,16);
        reliability.phase++; return;
    case 3:
        for(unsigned i=0;i<16;i++) verify.block[i]=reliability.link.skd_input[15u-i];
        tc_aes_encrypt(verify.stream,verify.block,&verify.aes);
        d->key_recomputed=(uint8_t)!memcmp(verify.stream,security.key,16);
        reliability.phase=6; return;
    case 6:
        ble_secure_zero(&verify,sizeof verify); reliability.phase=4; return;
    case 4: {
        /* Trials 0..9: C-2..C+2 in original and opposite directions;
         * 10: previous key/current IV; 11: previous key/previous IV, at C. */
        unsigned trial=reliability.trial; int offset=(int)(trial%5u)-2;
        uint64_t count=d->counter; int direction=d->direction;
        if(trial>=12) { reliability.phase++; return; }
        int valid=d->length>=5 && d->length<=31 && count<=BLE_CCM_LAST;
        if(trial<10) {
            if((offset<0 && count<(unsigned)-offset) ||
               (offset>0 && count>BLE_CCM_LAST-(unsigned)offset)) valid=0;
            else count=offset<0 ? count-(unsigned)-offset : count+(unsigned)offset;
            if(trial>=5) direction=!direction;
        } else valid=valid && reliability.previous_valid;
        if(!valid) { reliability.trial++; return; }
        if(trial<10) d->near_valid|=(uint16_t)(1u<<trial);
        if(!reliability.sw_active) {
            verify_begin(trial<10 ? security.key : reliability.previous_key,
                trial==11 ? reliability.previous_iv : security.iv,count,direction,
                (uint8_t)(d->header&3u),d->raw,d->length);
            reliability.sw_active=1; return;
        }
        if(!verify_step()) return;
        if(trial<10 && verify.pass) d->near_pass|=(uint16_t)(1u<<trial);
        if(trial==2) d->sw_same=verify.pass;
        if(trial==10) d->previous_key=verify.pass;
        if(trial==11) d->previous_context=verify.pass;
        ble_secure_zero(&verify,sizeof verify); reliability.sw_active=0; reliability.trial++; return;
    }
    default:
        reliability_dma(d,1); ble_port_aes_state(&d->after);
        d->complete=1; reliability.failure_active=0;
        /* Classifiers never deliver a PDU, alter a counter, or rescue a link. */
        reliability.closing_diag=1; ble_ll_close(0x3d); reliability.closing_diag=0; return;
    }
}
static int reliability_tx_pick(int best)
{
    if(reliability.pending.active) return reliability.pending.ready ? reliability.pending.id : -1;
    if(best<0 || !security.d.tx_on || !queues.tx[best].n) return best;
    /* Only precompute the next counter. It is committed by the hardware
     * refill, at the current TX toggle, with the original publish ordering. */
    if(security.d.tx_counter>BLE_CCM_LAST) return best;
    reliability.pending.active=1; reliability.pending.ready=0;
    reliability.pending.id=(uint8_t)best; reliability.pending.epoch=ble_link.epoch;
    reliability.pending.counter=security.d.tx_counter; reliability.pending.started=ble_port_ticks();
    queues.tx[best].state=3; ble_port_pend(); return -1;
}
static void reliability_tx_step(void)
{
    tx_t *t=&queues.tx[reliability.pending.id];
    if(!reliability.tx_sw_active) {
        uint8_t p[31]={0}; memcpy(p,t->data,t->n);
        verify_begin(security.key,security.iv,reliability.pending.counter,0,t->llid,p,(uint8_t)(t->n+4u));
        verify.encrypt=1; reliability.tx_sw_active=1; return;
    }
    if(!verify_step()) return;
    memcpy(reliability.pending.expected,verify.data,t->n+4u);
    ble_secure_zero(&verify,sizeof verify); reliability.tx_sw_active=0;
    reliability.pending.ready=1; queues.refill_pending=2; ble_port_pend();
}
static int reliability_tx_check(unsigned id,uint8_t *out,uint8_t n,uint64_t count,const ble_aes_state_t *before)
{
    int ok=reliability.pending.active && reliability.pending.ready && reliability.pending.id==id &&
        reliability.pending.epoch==ble_link.epoch && reliability.pending.counter==count &&
        !memcmp(out,reliability.pending.expected,n);
    uint32_t hold=ble_port_ticks()-reliability.pending.started;
    if(hold>reliability.stats.tx_hold_max_ticks) reliability.stats.tx_hold_max_ticks=hold;
    reliability.stats.tx_checked++; reliability.stats.tx_mismatches+=!ok;
    if(!ok && !reliability.tx.kind) {
        ble_reliability_record_t *d=&reliability.tx;
        reliability_context(d,2,count); d->direction=0; d->slot=255;
        d->length=n; d->header=(uint16_t)((n<<8)|queues.tx[id].llid);
        memcpy(d->raw,out,n); memcpy(d->raw_before,out,n); d->input_stable=1;
        d->before=*before; ble_port_aes_state(&d->failed); reliability.fault_dma=out;
        reliability_dma(d,0); reliability.fault=d; reliability.failure_active=1;
        reliability.phase=reliability.trial=reliability.sw_active=0; ble_radio_freeze(0x3d);
    }
    ble_secure_zero(&reliability.pending,sizeof reliability.pending);
    if(ok) { reliability.link.tx_ok=1; reliability_inputs_wipe(); }
    else if(!reliability.failure_active) ble_ll_close(0x3d);
    ble_port_pend(); return ok;
}
static int reliability_worker(void)
{
    int work=1; uint32_t start=ble_time_item_begin(),wall=ble_port_ticks();
    if(reliability.failure_active) reliability_failure_step();
    else {
        if((uint32_t)(ble_port_ms()-reliability.last_kat_ms)>=1000u && ble_time_has_room(60u*24u) && ble_port_diag_radio_idle()) {
            reliability.last_kat_ms=ble_port_ms(); (void)reliability_kat(); work=2;
        } else if(reliability.pending.active && !reliability.pending.ready) reliability_tx_step();
        else work=0;
    }
    if(work) {
        uint32_t dt=ble_time_own()-start,elapsed=ble_port_ticks()-wall;
        reliability.stats.steps++; reliability.stats.own_ticks+=dt; reliability.stats.over100+=dt>=2400u;
        if(dt>reliability.stats.max_ticks) reliability.stats.max_ticks=dt;
        if(elapsed>reliability.stats.max_wall_ticks) reliability.stats.max_wall_ticks=elapsed;
        ble_time_item_end(start,BLE_WORK_RELIABILITY,0); ble_port_pend();
        /* A background KAT need not delay ordinary link housekeeping. */
        return work==1 || (reliability.pending.active && !reliability.pending.ready);
    }
    return 0;
}
void ble_reliability_get(ble_reliability_stats_t *stats,ble_reliability_record_t *rx,ble_reliability_record_t *tx)
{
    uint32_t k=ble_port_lock(); *stats=reliability.stats; *rx=reliability.rx; *tx=reliability.tx; ble_port_unlock(k);
}
