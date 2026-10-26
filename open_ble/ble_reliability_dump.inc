/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Only public, explicitly enumerated fields are formatted. No memory dumps. */
static void r7_field(const char *name,uint32_t v)
{ BRD_PUTS(" "); BRD_PUTS(name); BRD_PUTS(" "); BRD_HEX(v,8); }
static void r7_bytes(const char *name,const uint8_t *p,unsigned n)
{
    BRD_PUTS(name); BRD_PUTS(" ");
    for(unsigned i=0;i<n;i++) BRD_HEX(p[i],2);
    BRD_PUTS("\n");
}
static void r7_aes(const char *name,const ble_aes_state_t *a)
{
    BRD_PUTS(name); r7_field("con",a->con); r7_field("epoch",a->epoch); r7_field("last",a->last_epoch);
    r7_field("ops",a->operations); r7_field("blocks",a->blocks); r7_field("timeouts",a->timeouts);
    r7_field("busy",a->busy); BRD_PUTS("\n");
}
static void r7_record(const ble_reliability_record_t *d,const char *source)
{
    BRD_PUTS("r7_record "); BRD_PUTS(source); r7_field("kind",d->kind); r7_field("complete",d->complete);
    r7_field("epoch",d->epoch); r7_field("ms",d->ms); BRD_PUTS("\n");
    if(!d->kind || !d->complete) return;
    BRD_PUTS("r7_pdu"); r7_field("header",d->header); r7_field("status",d->status);
    r7_field("event",d->event); r7_field("slot",d->slot); r7_field("dir",d->direction);
    BRD_PUTS(" counter "); BRD_HEX((uint32_t)(d->counter>>32),2); BRD_HEX((uint32_t)d->counter,8); BRD_PUTS("\n");
    r7_bytes("r7_ciphertext_tag",d->raw,d->length);
    r7_bytes("r7_input_before",d->raw_before,d->length);
    BRD_PUTS("r7_check"); r7_field("hw_retry",d->hw_retry); r7_field("sw_same",d->sw_same);
    r7_field("kat",d->kat); r7_field("near_valid",d->near_valid); r7_field("near_pass",d->near_pass); BRD_PUTS("\n");
    BRD_PUTS("r7_key"); r7_field("recomputed",d->key_recomputed); r7_field("unchanged",d->key_unchanged);
    r7_field("iv_unchanged",d->iv_unchanged); r7_field("inputs",d->inputs_retained);
    r7_field("derived",d->derived_valid); BRD_PUTS("\n");
    BRD_PUTS("r7_previous"); r7_field("valid",d->previous_valid); r7_field("key",d->previous_key);
    r7_field("context",d->previous_context); BRD_PUTS("\n");
    BRD_PUTS("r7_dma"); r7_field("valid0",d->dma_valid[0]); r7_field("valid1",d->dma_valid[1]);
    r7_field("equal",d->dma_equal); r7_field("input_stable",d->input_stable); r7_field("input0",d->dma_input_equal[0]);
    r7_field("input1",d->dma_input_equal[1]); BRD_PUTS("\n");
    for(unsigned i=0;i<2;i++) {
        BRD_PUTS("r7_dma_read"); r7_field("index",i); r7_field("header",d->dma_header[i]);
        r7_field("status",d->dma_status[i]); BRD_PUTS("\n");
        r7_bytes("r7_dma_ciphertext_tag",d->dma[i],d->length);
    }
    r7_bytes("r7_rand",d->rand_ediv,8); r7_bytes("r7_ediv",d->rand_ediv+8,2);
    r7_bytes("r7_skdm",d->skd,8); r7_bytes("r7_skds",d->skd+8,8);
    r7_bytes("r7_ivm",d->iv,4); r7_bytes("r7_ivs",d->iv+4,4); r7_bytes("r7_iv_used",d->iv_used,8);
    BRD_PUTS("r7_time_ms"); r7_field("connect_gap",d->connect_gap_ms);
    r7_field("prev_disconnect",d->previous_disconnect_ms); r7_field("enc_req",d->enc_req_age_ms);
    r7_field("start_ack",d->start_ack_age_ms); BRD_PUTS("\n");
    BRD_PUTS("r7_close_time_ms"); r7_field("connect_gap",d->connect_close_gap_ms);
    r7_field("prev_close",d->previous_close_ms); BRD_PUTS("\n");
    BRD_PUTS("r7_old_work"); r7_field("create",d->old_work_create); r7_field("enc",d->old_work_enc); BRD_PUTS("\n");
    r7_aes("r7_aes_before",&d->before); r7_aes("r7_aes_failed",&d->failed);
    r7_aes("r7_aes_retry",&d->after_retry); r7_aes("r7_aes_after",&d->after);
}
static void ble_reliability_dump(void)
{
    ble_reliability_stats_t stats; ble_reliability_record_t rx,tx;
    ble_reliability_get(&stats,&rx,&tx);
    BRD_PUTS("r7_diag 1 hex_fields results_0_fail_1_pass_2_unavailable\n");
    BRD_PUTS("r7_background"); r7_field("kat_runs",stats.kat_runs); r7_field("kat_failures",stats.kat_failures);
    r7_field("tx_checked",stats.tx_checked); r7_field("tx_mismatches",stats.tx_mismatches); BRD_PUTS("\n");
    BRD_PUTS("r7_cost_ticks24"); r7_field("steps",stats.steps); r7_field("max",stats.max_ticks);
    r7_field("over100",stats.over100); r7_field("wall_max",stats.max_wall_ticks); BRD_PUTS("\n");
    BRD_PUTS("r7_total_ticks24 "); BRD_HEX((uint32_t)(stats.own_ticks>>32),8); BRD_HEX((uint32_t)stats.own_ticks,8);
    r7_field("tx_hold_max",stats.tx_hold_max_ticks); BRD_PUTS("\n");
    r7_record(&rx,"rx"); r7_record(&tx,"tx"); BRD_PUTS("r7_end\n");
}

#undef BRD_PUTS
#undef BRD_DEC
#undef BRD_HEX
