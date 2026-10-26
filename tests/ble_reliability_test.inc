/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* All fault injection lives in this harness. Each case is a fresh process,
 * modelling power-on (.bss), whereas HCI Reset must retain the frozen records. */
#include "../open_ble/ble_timing.h"
#define BRD_PUTS ob_puts
#define BRD_DEC ob_dec
#define BRD_HEX ob_hex
#include "../open_ble/ble_reliability_dump.inc"
void (*sim_sw_aes_hook)(void);
static int r7_copy_fault,r7_key_fault;
extern uint32_t sim_sw_aes_cost_ticks,sim_sw_key_cost_ticks;
extern void sim_radio_corrupt(unsigned,unsigned);
extern void sim_radio_flip_tx_toggle(void);
int __real_ble_ccm(const uint8_t *,const uint8_t *,uint64_t,int,uint8_t,uint8_t *,uint8_t,int);
int __wrap_ble_ccm(const uint8_t *key,const uint8_t *v,uint64_t count,int direction,
                   uint8_t llid,uint8_t *p,uint8_t n,int decrypt)
{
    if(decrypt && r7_copy_fault) { p[0]^=1; r7_copy_fault=0; }
    if(decrypt && r7_key_fault) { ((uint8_t *)key)[0]^=1; r7_key_fault=0; }
    return __real_ble_ccm(key,v,count,direction,llid,p,n,decrypt);
}
static void r7_open(int reset,int bad_derive)
{
    ble_conn_t c=sim_connection(); uint8_t req[23]={3},ltk[16],skd[16],le[16];
    if(reset) { sim_reset(); sim_storage_reset(); sim_seed(); }
    c.interval=12; assert(ble_ll_create(&c,sim_ticks)); sim_radio_start();
    sn=expected_sn=0; next_counter=accepted_counter=0; sim_ticks=ble_link.time.first;
    for(unsigned i=0;i<8;i++) req[11+i]=(uint8_t)(i+71u);
    for(unsigned i=0;i<10;i++) req[1+i]=(uint8_t)(i+0x20u);
    for(unsigned i=0;i<4;i++) req[19+i]=(uint8_t)(i+0x30u);
    direct(req,23,3,0,0);
    for(unsigned i=0;i<16;i++) { ble_entropy_step(); work(); }
    int slot=opcode(4); assert(slot>=0);
    memcpy(skd,req+11,8); memcpy(skd+8,sim_air[slot]+1,8);
    memcpy(iv,req+19,4); memcpy(iv+4,sim_air[slot]+9,4);
    for(unsigned i=0;i<16;i++) ltk[i]=(uint8_t)i;
    assert(ble_aes_le(ltk,skd,le)); for(unsigned i=0;i<16;i++) sk[i]=le[15u-i];
    sim_ack((unsigned)slot);
    sim_aes_wrong_blocks=bad_derive;
    assert(!ble_ll_ltk_reply(BLE_HANDLE,ltk,0)); work();
    sim_aes_wrong_blocks=0;
    slot=opcode(5); assert(slot>=0); sim_ack((unsigned)slot);
}
static void r7_send(uint8_t *air,uint8_t length)
{
    assert(sim_radio_receive(0x7901,(uint16_t)((length<<8)|3u|(sn<<3)|4u),air,7));
    sn^=1; sim_radio_service(0);
}
static void r7_audio_preempt(void)
{
    ble_time_preempt_enter(); sim_ticks+=186u*24u; ble_time_preempt_leave();
}
static void r7_dma_change(void) { sim_radio_corrupt(1,0); }
static void r7_test(const char *name)
{
    ble_reliability_stats_t stats;
    ble_reliability_record_t rx,tx,saved;
    uint8_t air[31]={18},old_key[16],old_iv[8]; uint64_t counter=0,used=0; int dir=1;
    if(!strcmp(name,"old-command")) {
        begin(); uint8_t cmd[21]={0x1a,0x20,18,0x50,0}; assert(!ble_hci_send(1,cmd,sizeof cmd));
        ble_ll_close(0x13); work();
        uint8_t type,p[80]; uint16_t n; while(ble_ll_poll(&type,p,&n)) { }
        sim_ticks+=131000u*24u; r7_open(0,0);
        air[0]=6; assert(central_ccm(0,1,3,air,1,0)); air[4]^=1;
    } else if(!strcmp(name,"previous-key") || !strcmp(name,"previous-context")) {
        begin(); memcpy(old_key,sk,16); memcpy(old_iv,iv,8);
        ble_ll_close(0x13); work();
        uint8_t type,p[80]; uint16_t n; while(ble_ll_poll(&type,p,&n)) { }
        assert(ble_link.state==BLE_LINK_IDLE); sim_ticks+=131000u*24u; r7_open(0,0);
        uint8_t current_key[16],current_iv[8]; memcpy(current_key,sk,16); memcpy(current_iv,iv,8);
        memcpy(sk,old_key,16); if(!strcmp(name,"previous-context")) memcpy(iv,old_iv,8);
        air[0]=6; assert(central_ccm(0,1,3,air,1,0));
        memcpy(sk,current_key,16); memcpy(iv,current_iv,8);
    } else if(!strcmp(name,"derive-block") || !strcmp(name,"key-byte") || !strcmp(name,"counter-zero")) {
        r7_open(1,!strcmp(name,"derive-block")); air[0]=6;
        if(!strcmp(name,"key-byte")) r7_key_fault=1;
        used=!strcmp(name,"counter-zero") ? 1 : 0;
        assert(central_ccm(used,1,3,air,1,0));
    } else {
        begin(); counter=next_counter; used=counter;
        if(!strncmp(name,"counter",7)) {
            counter=64; used=(uint64_t)((int64_t)counter+strtol(name+7,0,10));
            if(!strcmp(name,"counter-max")) counter=used=(((uint64_t)1<<39)-1u);
            ble_security_diag_t d; ble_security_get_diag(&d); ble_security_test_counters(d.tx_counter,counter);
        }
        if(!strcmp(name,"direction")) dir=0;
        assert(central_ccm(used,dir,3,air,1,0));
        if(!strcmp(name,"counter-max")) air[4]^=1; /* classify a bad MIC at the upper bound */
    }
    if(!strcmp(name,"tx-toggle")) {
        uint8_t acl[5]={0x50,0x20,1,0,0x99};
        ble_security_diag_t enc; ble_security_get_diag(&enc); uint64_t first=enc.tx_counter;
        assert(!ble_ll_acl(acl,sizeof acl)); acl[4]=0x88; assert(!ble_ll_acl(acl,sizeof acl));
        sim_radio_flip_tx_toggle(); work();
        unsigned seen=0;
        for(unsigned slot=0;slot<2;slot++) if(sim_slot[slot]>=0) {
            uint8_t p[31]; memcpy(p,sim_air[slot],sim_air_len[slot]);
            uint64_t c=ble_ll_tx_data((unsigned)sim_slot[slot])[0]==0x99 ? first : first+1u;
            assert(central_ccm(c,0,2,p,1,1)); seen++;
        }
        assert(seen==2); ble_reliability_get(&stats,&rx,&tx);
        assert(!stats.tx_mismatches && !rx.kind && !tx.kind);
        puts("R7 automatic-empty toggle during software precompute: current-slot publication, two consecutive counters: PASS"); return;
    }
    if(!strcmp(name,"kat-under-load")) {
        uint8_t acl[5]={0x50,0x20,1,0,0x99};
        ble_reliability_get(&stats,&rx,&tx); uint32_t kat=stats.kat_runs;
        assert(!ble_ll_acl(acl,sizeof acl)); sim_ticks+=24000000u;
        ble_ll_worker(); ble_reliability_get(&stats,&rx,&tx); assert(stats.kat_runs==kat+1u);
        work(); ble_reliability_get(&stats,&rx,&tx); assert(!stats.tx_mismatches);
        puts("R7 periodic KAT has priority over continuous TX precompute: PASS"); return;
    }
    if(!strcmp(name,"background")) {
        for(unsigned i=0;i<3;i++) { sim_ticks+=24000000u; work(); }
        ble_reliability_get(&stats,&rx,&tx); assert(stats.kat_runs>=3 && !stats.kat_failures);
        sim_aes_wrong_blocks=-1; sim_ticks+=24000000u; work(); sim_aes_wrong_blocks=0;
        ble_reliability_get(&stats,&rx,&tx); assert(stats.kat_failures==1);
        assert(stats.tx_checked>=1 && !stats.tx_mismatches && !rx.kind && !tx.kind);
        puts("R7 periodic KAT outside radio IRQ + every encrypted TX verified: PASS"); return;
    }
    sim_sw_aes_cost_ticks=75u*24u; sim_sw_key_cost_ticks=20u*24u;
    sim_memory_cost_ticks=1; sim_aes_cost_ticks=24;
    if(!strcmp(name,"copy-byte")) r7_copy_fault=1;
    if(!strcmp(name,"dma-byte")) { air[4]^=1; sim_sw_aes_hook=r7_dma_change; }
    if(!strcmp(name,"aes-transient")) sim_aes_wrong_blocks=1;
    if(!strcmp(name,"audio-preempt")) { sim_aes_wrong_blocks=1; sim_sw_aes_hook=r7_audio_preempt; }
    if(!strcmp(name,"aes-persistent")) sim_aes_wrong_blocks=-1;
    if(!strcmp(name,"tx-block")) {
        uint8_t acl[5]={0x50,0x20,1,0,0x99}; sim_aes_wrong_blocks=1;
        assert(!ble_ll_acl(acl,sizeof acl));
        /* The known-bad result is still unpublished. */
        for(unsigned j=0;j<2;j++) assert(sim_air_len[j]!=5 || sim_slot[j]<0);
    } else if(!strcmp(name,"phase")) ble_ll_close(0x3d);
    else r7_send(air,5);
    /* Each invocation is an executor pass, with charged software AES cost.
     * This is a scheduling bound test, not a silicon timing measurement. */
    for(unsigned i=0;i<512;i++) {
        ble_time_pass_begin(sim_ticks); ble_ll_worker(); ble_time_pass_end();
        ble_reliability_get(&stats,&rx,&tx);
        if(rx.complete || tx.complete) break;
    }
    sim_aes_wrong_blocks=0;
    ble_reliability_get(&stats,&rx,&tx);
    ble_reliability_record_t *d=!strcmp(name,"tx-block") ? &tx : &rx;
    assert(d->complete && d->kind && ble_link.reason==0x3d);
    if(stats.over100 || stats.max_ticks>=2400u) { fprintf(stderr,"R7 cost %s max=%u over=%u\n",name,stats.max_ticks,stats.over100); }
    assert(!stats.over100 && stats.max_ticks<2400u);
    if(!strcmp(name,"old-command")) assert(d->old_work_create==384u && d->old_work_enc==384u);
    else assert(!d->old_work_create && !d->old_work_enc);
    if(!strcmp(name,"audio-preempt")) assert(stats.max_wall_ticks>=261u*24u && !stats.over100);
    if(strcmp(name,"phase")) {
        assert(d->counter==counter && d->dma_valid[0] && d->dma_valid[1]);
        assert(d->key_unchanged==(!strcmp(name,"key-byte") ? 0 : 1));
        assert(d->iv_unchanged==1);
    }
    if(!strncmp(name,"counter",7) && strcmp(name,"counter-max")) {
        unsigned bit=!strcmp(name,"counter-zero") ? 3u : (unsigned)(strtol(name+7,0,10)+2);
        assert(d->near_pass==(1u<<bit) && !d->sw_same && !d->hw_retry);
    }
    if(!strcmp(name,"counter-zero")) assert(d->near_valid==0x39cu && d->key_recomputed==1);
    if(!strcmp(name,"counter-max")) assert(d->near_valid==0xe7u && !d->near_pass);
    if(!strcmp(name,"direction")) assert(d->near_pass==(1u<<7) && !d->sw_same);
    if(!strcmp(name,"previous-key")) assert(d->previous_key==1 && !d->sw_same && d->connect_gap_ms>=131);
    if(!strcmp(name,"previous-context")) assert(d->previous_context==1 && !d->previous_key);
    if(!strcmp(name,"copy-byte")) assert(!d->input_stable && !d->dma_input_equal[0] && d->dma_equal);
    if(!strcmp(name,"dma-byte")) assert(!d->dma_equal && d->dma_input_equal[0] && !d->dma_input_equal[1]);
    if(!strcmp(name,"aes-transient")) assert(d->hw_retry==1 && d->sw_same==1 && d->kat==1);
    if(!strcmp(name,"aes-persistent")) assert(!d->hw_retry && d->sw_same==1 && !d->kat);
    if(!strcmp(name,"key-byte")) assert(!d->key_recomputed && !d->key_unchanged);
    if(!strcmp(name,"derive-block")) assert(!d->key_recomputed && d->key_unchanged);
    if(!strcmp(name,"tx-block")) assert(stats.tx_mismatches==1 && !d->sw_same && d->kat==1 && d->dma_equal);
    saved=*d;
    dump_used=0; ble_reliability_dump();
    unsigned line=0; for(const char *p=dump;*p;p++) { if(*p=='\n') line=0; else assert(++line<188); }
    /* Known secrets are deliberately recognizable, and must not be formatted. */
    assert(!strstr(dump,"000102030405060708090A0B0C0D0E0F"));
    char keyhex[33]; for(unsigned i=0;i<16;i++) snprintf(keyhex+2*i,3,"%02X",sk[i]);
    assert(!strstr(dump,keyhex));
    ble_ll_reset(); begin();
    ble_reliability_get(&stats,&rx,&tx); d=!strcmp(name,"tx-block") ? &tx : &rx;
    assert(!memcmp(&saved,d,sizeof saved));
    printf("R7 %-16s hw=%u sw=%u near=%03x key=%u/%u prev=%u/%u dma=%u/%u/%u KAT=%u max_model_ticks=%u frozen across Reset/reconnect: PASS\n",
        name,d->hw_retry,d->sw_same,d->near_pass,d->key_recomputed,d->key_unchanged,
        d->previous_key,d->previous_context,d->dma_equal,d->dma_input_equal[0],d->dma_input_equal[1],d->kat,stats.max_ticks);
}
