/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
/* Complete original counters over the existing USB log protocol. Long logical
 * lines continue in the next frame (187 ASCII bytes/frame), never truncate. */
static void log_line_send(const char *line);
static char ob_line[188];
static unsigned ob_used;
static void ob_flush(void)
{
    if(!ob_used) return;
    ob_line[ob_used]=0; log_line_send(ob_line); ob_used=0;
    fm1_wdt_feed(); ble_poll();
}
static void ob_putc(char c)
{
    if(c=='\r') return;
    if(c=='\n') { ob_flush(); return; }
    if(ob_used==sizeof ob_line-1u) ob_flush();
    ob_line[ob_used++]=c;
}
static void ob_puts(const char *p) { while(*p) ob_putc(*p++); }
static void ob_hex(uint32_t v,unsigned n)
{
    while(n--) ob_putc("0123456789ABCDEF"[(v>>(n*4u))&15u]);
}
static void ob_dec(int32_t v)
{
    char b[10]; unsigned n=0; uint32_t u=(uint32_t)v;
    if(v<0) { ob_putc('-'); u=0u-u; }
    do { b[n++]=(char)('0'+u%10u); u/=10u; } while(u);
    while(n) ob_putc(b[--n]);
}
static void ob_kv(const char *name,int32_t v) { ob_puts(name); ob_putc(' '); ob_dec(v); ob_putc('\n'); }
static void ob_kx(const char *name,uint32_t v) { ob_puts(name); ob_putc(' '); ob_hex(v,8); ob_putc('\n'); }
/* The compact, frozen-at-entry readout is available in release builds too.
 * No RF polling or trace transfer; USB remains serviced between rows. */
static void ob_gap(const char *label,const ble_gap_event_t *e)
{
    static const char *const states[]={"off","starting","advertising","link_up"};
    ob_puts(label); ob_puts(" ms "); ob_dec((int32_t)e->ms);
    ob_puts(" us "); ob_dec((int32_t)(e->ticks/24u));
    ob_puts(" state "); ob_puts(states[e->state]);
    ob_puts(" end_state "); ob_puts(states[e->end_state]);
    ob_puts(" reason ");
    if(!e->reasons) ob_puts("unknown");
    if(e->reasons&BLE_GAP_BOOT) ob_puts("boot,");
    if(e->reasons&BLE_GAP_RADIO_START) ob_puts("radio_start,");
    if(e->reasons&BLE_GAP_RADIO_STOP) ob_puts("radio_stop,");
    if(e->reasons&BLE_GAP_FLASH) ob_puts("flash_write,");
    ob_puts(" epoch "); ob_dec((int32_t)e->epoch); ob_putc('\n');
}
/* Freeze identity metadata before USB output yields to host callbacks. */
static void open_ble_bonds_dump(void)
{
    ble_bond_diag_t d; ble_bond_peer_diag_t peer[BLE_BOND_CAPACITY]; unsigned count=0;
    ble_bonds_get_diag(&d);
    while(count<BLE_BOND_CAPACITY && !ble_bonds_peer(count,&peer[count])) count++;
    ob_kv("bond_count",d.count); ob_kv("bond_capacity",d.capacity);
    ob_kv("bond_replacements",(int32_t)d.replacements);
    for(unsigned i=0;i<count;i++) {
        const ble_bond_peer_diag_t *p=&peer[i];
        ob_puts("bond_identity ");
        for(unsigned n=6;n>0;n--) { ob_hex(p->addr[n-1u],2); if(n>1) ob_putc(':'); }
        ob_puts(" type "); ob_dec(p->type); ob_puts(" rank "); ob_dec(p->rank);
        ob_puts(p->sc ? " SC" : " legacy");
        for(unsigned c=0;c<2;c++) {
            ob_puts(" cccd "); ob_hex(p->cccd_handle[c],4); ob_putc('='); ob_hex(p->cccd_flags[c],4);
        }
        ob_putc('\n');
    }
}
#if FM1B_BLE_DIAG
#define BRD_PUTS ob_puts
#define BRD_DEC ob_dec
#define BRD_HEX ob_hex
#include "../open_ble/ble_radio_dump.inc"
#if FM1B_BLE_RELIABILITY
#define BRD_PUTS ob_puts
#define BRD_DEC ob_dec
#define BRD_HEX ob_hex
#include "../open_ble/ble_reliability_dump.inc"
#endif
static void open_ble_mic_dump(void)
{
    /* Snapshot before USB output yields: failures cannot tear a readout. */
    ble_mic_record_t records[BLE_MIC_RECORDS];
    unsigned count=ble_security_mic_records(records);
    ob_kv("mic_records",(int32_t)count);
    for(unsigned i=0;i<count;i++) {
        const ble_mic_record_t *d=&records[i];
        ob_puts("mic_record serial "); ob_dec((int32_t)d->serial);
        ob_puts(" ms "); ob_dec((int32_t)d->ms);
        ob_puts(" epoch "); ob_dec((int32_t)d->epoch);
        ob_puts(" received_ms "); ob_dec((int32_t)d->received_ms);
        ob_puts(" event "); ob_dec(d->event);
        ob_puts(" service_event "); ob_dec(d->service_event);
        ob_puts(" interval "); ob_dec(d->interval);
        ob_puts(" since_update_ms "); ob_hex(d->since_update_ms,8); ob_putc('\n');
        ob_puts("mic_pdu serial "); ob_dec((int32_t)d->serial);
        ob_puts(" slot "); ob_dec(d->slot);
        ob_puts(" flags "); ob_hex(d->flags,2);
        ob_puts(" status "); ob_hex(d->status,4);
        ob_puts(" llid "); ob_dec(d->header&3u);
        ob_puts(" sn "); ob_dec((d->header>>3)&1u);
        ob_puts(" nesn "); ob_dec((d->header>>2)&1u);
        ob_puts(" md "); ob_dec((d->header>>4)&1u);
        ob_puts(" length "); ob_dec(d->header>>8);
        ob_puts(" counter "); ob_hex((uint32_t)(d->counter>>32),2); ob_hex((uint32_t)d->counter,8); ob_putc('\n');
        for(unsigned j=0;j<d->rx_count+d->tx_count;j++) {
            int rx=j<d->rx_count;
            const ble_mic_pdu_t *p=rx ? &d->rx[j] : &d->tx[j-d->rx_count];
            ob_puts(rx ? "mic_rx serial " : "mic_tx serial "); ob_dec((int32_t)d->serial);
            ob_puts(" index "); ob_dec((int32_t)(rx ? j : j-d->rx_count));
            ob_puts(" sn "); ob_dec(p->sn);
            ob_puts(" counter "); ob_hex((uint32_t)(p->counter>>32),2); ob_hex((uint32_t)p->counter,8);
            ob_puts(" length "); ob_dec(p->length); ob_putc('\n');
        }
    }
}
static void open_ble_events_dump(void)
{
    static const char *const names[]={"unknown","connect","disconnect","enc_start","enc_refresh",
        "smp_start","smp_result","mtu","cccd_write","cccd_restore","sc_sent","sc_confirmed",
        "l2cap_request","l2cap_result","conn_update","midi_read","att_reject"};
    /* Freeze before emitting rows: USB sends yield to the host. */
    ble_host_event_t events[BLE_HOST_EVENT_COUNT];
    unsigned count=ble_host_events(events);
    for(unsigned i=0;i<count;i++) {
        const ble_host_event_t *e=events+i;
        ob_puts("ble_event ms "); ob_dec((int32_t)e->ms);
        ob_puts(" session "); ob_dec((int32_t)e->session);
        ob_puts(" conn "); ob_dec(e->connection);
        ob_putc(' '); ob_puts(e->kind<sizeof names/sizeof names[0] ? names[e->kind] : names[0]);
        ob_puts(" a "); ob_dec((int32_t)e->a);
        ob_puts(" b "); ob_dec((int32_t)e->b);
        ob_puts(" c "); ob_dec((int32_t)e->c); ob_putc('\n');
    }
}
#endif
static void open_ble_audio_dump(void);
static void open_ble_health_dump(void)
{
    ble_health_t h; ble_link_diag_t l; ble_host_diag_t host; ble_timing_diag_t t;
    ble_security_diag_t enc; ble_bond_diag_t bond;
    uint32_t k=fm1_ble_irq_save(),ms=ble_health_ms(fm1_ticks()); h=ble_health;
    ble_ll_get_diag(&l); fm1_ble_irq_restore(k);
    ble_host_get_diag(&host); ble_time_get_diag(&t);
    ble_security_get_diag(&enc); ble_bonds_get_diag(&bond);
    ob_kv("ble_diag",FM1B_BLE_DIAG); ob_kv("sample_ms",(int32_t)ms);
    ob_kv("peripheral_state",plat_ble_state()); ob_kv("peripheral_linked",plat_ble_linked());
    ob_kv("link_state_idle_created_up_closing_stopped",l.state); ob_kv("link_epoch",(int32_t)l.epoch);
    ob_kv("conn_interval_1250us",l.interval); ob_kv("conn_latency",l.latency); ob_kv("conn_timeout_10ms",l.timeout);
    ob_kv("conn_event",l.event); ob_kv("conn_events",(int32_t)l.events); ob_kv("conn_updates",(int32_t)l.updates);
    ob_kv("pending_kind",l.instant_kind); ob_kv("supervision_fail",(int32_t)l.supervision_fail);
    ob_kv("late_instants",(int32_t)l.late_instants); ob_kv("disconnects",(int32_t)l.disconnects);
    ob_kv("enc_enabled",enc.encrypted); ob_kv("mic_failures",(int32_t)enc.mic_failures);
    ob_kv("nimble_bonded",host.bonded); ob_kv("nimble_subscribed",host.subscribed); ob_kv("nimble_mtu",host.mtu);
    ob_kv("nimble_resets",(int32_t)host.resets); ob_kv("nimble_error",(int32_t)host.error);
    ob_kv("param_pending",host.param_pending); ob_kv("param_requests",(int32_t)host.param_requests); ob_kv("param_status",(int32_t)host.param_status);
    ob_kv("gatt_invalidations",(int32_t)host.gatt_invalidations);
    ob_kv("gatt_confirmations",(int32_t)host.gatt_confirmations);
    ob_kv("gatt_peer_version",(int32_t)host.gatt_peer_version);
    ob_kv("bond_pending",bond.pending); ob_kv("bond_failures",(int32_t)bond.failures);
    open_ble_bonds_dump();
    ob_kv("midi_packets_in",(int32_t)host.midi_in); ob_kv("midi_packets_out",(int32_t)host.midi_out);
    ob_kv("midi_bad",(int32_t)host.midi_bad); ob_kv("midi_drop",(int32_t)host.midi_drop);
    ob_kv("error",(int32_t)br_diag.error); ob_kv("dma_canary_errors",(int32_t)br_diag.canary_errors);
    ob_kv("executor_selftest",(int32_t)br_diag.executor_selftest);
    ob_kv("executor_dispatch_live_max_us",(int32_t)(t.dispatch_live_max/24u));
    ob_kv("executor_item_max_us",(int32_t)(t.item_max/24u)); ob_kv("executor_item_over_100us",(int32_t)t.item_over_100us);
    ob_kv("executor_own_over_250us",(int32_t)t.own_over_250us); ob_kv("executor_over_250us_plus_item",(int32_t)t.pass_over_budget);
    ob_kv("refill_max_us",(int32_t)(t.refill_max/24u)); ob_kv("refill_over_80us",(int32_t)t.refill_over_80us);
    ob_kv("worker_timer_calls",(int32_t)h.calls); ob_kv("worker_timer_max_gap_us",(int32_t)(h.maximum.ticks/24u));
    ob_kv("worker_timer_over_7500us",(int32_t)h.over); ob_gap("worker_gap_max",&h.maximum);
    for(unsigned i=0;i<h.count;i++) ob_gap("worker_gap",&h.last[(h.next+4u-h.count+i)%4u]);
    ob_kv("adv_session",(int32_t)h.adv.session); ob_kv("connect_seen",(int32_t)h.adv.seen);
    ob_kv("connect_accepted",(int32_t)h.adv.accepted);
#if FM1B_BLE_DIAG
    for(unsigned i=0,n=h.adv.session<4u ? h.adv.session : 4u;i<n;i++) {
        const ble_adv_session_t *a=&h.sessions[(h.adv.session-n+i)%4u];
        ob_puts("adv_session_record id "); ob_dec((int32_t)a->session);
        ob_puts(" ms "); ob_dec((int32_t)a->ms); ob_puts(" seen "); ob_dec((int32_t)a->seen);
        ob_puts(" accepted "); ob_dec((int32_t)a->accepted); ob_putc('\n');
    }
#endif
    open_ble_audio_dump();
#if FM1B_BLE_DIAG
    open_ble_events_dump();
    open_ble_mic_dump();
#if FM1B_BLE_RELIABILITY
    ble_reliability_dump();
#endif
    ble_radio_dump();
#endif
}
#if FM1B_BLE_DIAG
/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wall"
#pragma clang diagnostic error "-Wextra"
/* Original S1-S3.1 status rows, now over fm1-base USB log. Main only. */
static ble_hw_diag_t ble_frozen_hw;
static ble_diag_t ble_frozen_ctl;
static ble_timing_diag_t ble_frozen_timing;
static uint8_t ble_console_frozen;

static void ob_ble_bytes(const uint8_t *p,unsigned n)
{
    unsigned i;
    for (i=0;i<n;i++) { ob_hex(p[i],2); if (i+1<n) ob_putc(' '); }
    ob_puts("\r\n");
}
static void ob_ble_field(const char *name,uint32_t value)
{
    ob_putc(' '); ob_puts(name); ob_putc(' '); ob_dec((int32_t)value);
}
static void open_ble_dump(void)
{
    open_ble_health_dump();
    ble_hw_diag_t hw;
    ble_diag_t d;
    unsigned i;
    if (ble_console_frozen) { hw=ble_frozen_hw; d=ble_frozen_ctl; }
    else {
        uint32_t k=fm1_ble_irq_save(); hw=br_diag; fm1_ble_irq_restore(k);
        ble_get_diag(&d);
    }
#if FELUCCA_BLE_STAGE >= 3
#if FELUCCA_BLE_S31
    ob_puts("BLE S3.1 / NimBLE 1.10.0 / MIDI; SC Just Works + legacy\r\n");
#else
    ob_puts("BLE S3 / NimBLE 1.10.0 / MIDI; SC Just Works + legacy\r\n");
#endif
    {
        ble_security_diag_t enc; ble_seed_diag_t seed; ble_bond_diag_t bond;
        ble_security_get_diag(&enc); ble_seed_get_diag(&seed); ble_bonds_get_diag(&bond);
        ob_kv("enc_phase_N_S1_S2_PA_PB_PC",enc.phase); ob_kv("enc_rx_on",enc.rx_on); ob_kv("enc_tx_on",enc.tx_on);
        ob_kv("enc_enabled",enc.encrypted); ob_kv("enc_starts",(int32_t)enc.starts); ob_kv("enc_refreshes",(int32_t)enc.refreshes);
        ob_kv("mic_failures",(int32_t)enc.mic_failures); ob_kv("enc_phase_errors",(int32_t)enc.phase_errors);
        ob_kv("ltk_requests",(int32_t)enc.ltk_requests); ob_kv("ltk_negative",(int32_t)enc.negative_replies);
        ob_kv("enc_tx_counter_hi",(int32_t)(enc.tx_counter>>32)); ob_kx("enc_tx_counter_lo",(uint32_t)enc.tx_counter);
        ob_kv("enc_rx_counter_hi",(int32_t)(enc.rx_counter>>32)); ob_kx("enc_rx_counter_lo",(uint32_t)enc.rx_counter);
        ob_kv("seed_FAILED_FIRST_CHAINED_PROVISIONED_UNPERSISTED",seed.state); ob_kv("seed_origin",seed.origin);
        ob_kv("seed_STORED_NOT_STORED_UNVERIFIED",seed.persistence); ob_kx("seed_sequence",seed.sequence);
        ob_kv("seed_credit_bits",seed.credit); ob_kv("seed_malformed",(int32_t)seed.malformed); ob_kv("seed_conflicts",(int32_t)seed.conflicts);
        ob_kv("seed_flash_failures",(int32_t)seed.flash_failures); ob_kv("le_rand_calls",(int32_t)seed.rand_calls);
        ob_kv("entropy_bytes",(int32_t)seed.bytes); ob_kv("reseed_attempts",(int32_t)seed.reseed_attempts);
        open_ble_bonds_dump();
        ob_kv("bond_records",bond.records); ob_kv("bond_pending",bond.pending); ob_kv("bond_writes",(int32_t)bond.writes);
        ob_kv("bond_erases",(int32_t)bond.erases); ob_kv("bond_failures",(int32_t)bond.failures); ob_kv("bond_malformed",(int32_t)bond.malformed);
        ob_kv("bond_private_refused",(int32_t)bond.private_refused);
        ob_kv("bond_private_dropped",(int32_t)bond.private_dropped);
        ob_kv("bond_orphan_dropped",(int32_t)bond.orphan_dropped);
        ob_kv("bond_equal_writes_skipped",(int32_t)bond.net_noops);
    }
#if FELUCCA_BLE_S31
    {
        ble_privacy_diag_t privacy; ble_privacy_get_diag(&privacy);
        ob_kv("rl_capacity",privacy.capacity); ob_kv("rl_entries",privacy.count); ob_kv("rl_enabled",privacy.enabled);
        ob_kv("rl_attempts",(int32_t)privacy.attempts); ob_kv("rl_matches",(int32_t)privacy.matches);
        ob_kv("rl_unresolved",(int32_t)privacy.unresolved); ob_kv("rl_aes_failures",(int32_t)privacy.aes_failures);
        ob_kv("rl_network_rejected",(int32_t)privacy.network_rejected);
    }
#endif
#else
    ob_puts("BLE S2.1 / NimBLE 1.10.0 / MIDI; encryption off\r\n");
#endif
    {
        ble_timing_diag_t timing;
        if(ble_console_frozen) timing=ble_frozen_timing; else ble_time_get_diag(&timing);
        ble_breadcrumb_t b; uint32_t timer[8],k=fm1_ble_irq_save();
        b=fm1_ble_breadcrumb; fm1_ble_worker_timer_readback(timer);
        timer[4]=ble_health.calls; timer[5]=ble_health.maximum.ticks;
        timer[6]=ble_health.over; timer[7]=ble_health.last_ticks;
        fm1_ble_irq_restore(k);
        ob_kv("executor_selftest",(int32_t)hw.executor_selftest);
        ob_kv("executor_dispatch_max_us",(int32_t)(timing.dispatch_max/24u));
        ob_kv("executor_dispatch_live_max_us",(int32_t)(timing.dispatch_live_max/24u));
        ob_kv("executor_dispatch_negative",(int32_t)timing.dispatch_negative);
        ob_kv("executor_wall_max_us",(int32_t)(timing.wall_max/24u));
        ob_kv("executor_own_max_us",(int32_t)(timing.own_max/24u));
        ob_kv("executor_item_max_us",(int32_t)(timing.item_max/24u));
        ob_kv("executor_item_over_100us",(int32_t)timing.item_over_100us);
        ob_puts("executor_item_max_kind "); ob_puts(ble_time_kind_name(timing.item_kind)); ob_puts("\r\n");
        ob_kx("executor_item_max_opcode",timing.item_opcode);
        for(i=0;i<BLE_WORK_COUNT;i++) {
            ob_puts("executor_kind "); ob_puts(ble_time_kind_name(i));
            ob_puts(" max_ticks "); ob_dec((int32_t)timing.kind_max[i]);
            ob_puts(" count "); ob_dec((int32_t)timing.kind_count[i]);
            ob_puts(" over100 "); ob_dec((int32_t)timing.kind_over_100us[i]);
            ob_puts(" opcode "); ob_hex(timing.kind_opcode[i],4); ob_puts("\r\n");
        }
        ob_kv("executor_own_over_250us",(int32_t)timing.own_over_250us);
        ob_kv("executor_over_250us_plus_item",(int32_t)timing.pass_over_budget);
        ob_kv("refill_max_us",(int32_t)(timing.refill_max/24u));
        ob_kv("refill_ccm_max_us",(int32_t)(timing.refill_ccm_max/24u));
        ob_kv("refill_other_max_us",(int32_t)(timing.refill_other_max/24u));
        ob_kv("refill_over_80us",(int32_t)timing.refill_over_80us);
        ob_kv("worker_timer_calls",(int32_t)timer[4]);
        ob_kv("worker_timer_max_gap_us",(int32_t)(timer[5]/24u));
        ob_kv("worker_timer_over_7500us",(int32_t)timer[6]);
        ob_kx("worker_timer_last_ticks",timer[7]);
        ob_kx("worker_timer_con",timer[0]); ob_kv("worker_timer_cnt",(int32_t)timer[1]);
        ob_kv("worker_timer_prd",(int32_t)timer[2]); ob_kx("worker_timer_irqcfg",timer[3]);
        ob_kx("ble_breadcrumb",b.last); ob_kx("ble_breadcrumb_ticks",b.last_ticks);
        ob_kx("ble_fault_breadcrumb",b.fault); ob_kx("ble_fault_ticks",b.fault_ticks);
        ob_kv("ble_fault_crash_count",(int32_t)b.fault_count);
    }
    {
        ble_link_diag_t link; ble_host_diag_t host;
        ble_ll_get_diag(&link); ble_host_get_diag(&host);
        ob_kv("link_state_idle_created_up_closing_stopped",link.state);
        ob_kv("link_epoch",(int32_t)link.epoch); ob_kv("link_established",link.established);
        ob_kv("conn_interval_1250us",link.interval); ob_kv("conn_latency",link.latency);
        ob_kv("conn_timeout_10ms",link.timeout); ob_kv("conn_event",link.event);
        ob_kv("connections",(int32_t)link.created); ob_kv("connect_invalid",(int32_t)link.invalid);
        ob_kv("conn_events",(int32_t)link.events);
        ob_kv("rx_A_new",(int32_t)link.rx_class[0]); ob_kv("rx_B_crc_5_13",(int32_t)link.rx_class[1]);
        ob_kv("rx_C_repeat_9",(int32_t)link.rx_class[2]); ob_kv("rx_D_unknown",(int32_t)link.rx_class[3]);
        ob_kv("retransmission_rx",(int32_t)link.repetitions);
        ob_kv("rx_delayed_stamp",(int32_t)link.rx_delayed);
        ob_kv("rx_backpressure",(int32_t)link.rx_backpressure); ob_kv("rx_queued",link.rx_used); ob_kv("rx_peak",link.rx_peak);
        ob_kv("tx_queued_loaded",link.tx_used); ob_kv("establishment_fail",(int32_t)link.establishment_fail);
        ob_kv("supervision_fail",(int32_t)link.supervision_fail); ob_kv("disconnects",(int32_t)link.disconnects);
        ob_kx("last_disconnect_reason",link.reason); ob_kv("conn_updates",(int32_t)link.updates);
        ob_kv("channel_map_updates",(int32_t)link.map_updates); ob_kv("late_instants",(int32_t)link.late_instants);
        ob_puts("pending_proc"); ob_ble_field("kind",link.instant_kind);
        ob_ble_field("instant",link.instant); ob_ble_field("received",link.received);
        ob_ble_field("programmed",link.programmed); ob_ble_field("interval",link.pending.interval);
        ob_ble_field("latency",link.pending.latency); ob_ble_field("timeout",link.pending.timeout);
        ob_ble_field("win",link.pending.win); ob_ble_field("offset",link.pending.offset);
        ob_puts(" map "); ob_ble_bytes(link.pending.map,5);
        ob_puts("control_last_ignored"); ob_ble_field("opcode",link.ignored_opcode);
        ob_ble_field("reason",link.ignored_reason); ob_ble_field("event",link.ignored_event); ob_puts("\r\n");
        for(i=1;i<BLE_IGNORE_COUNT;i++) {
            static const char *const names[]={"none","length","params","map","pending","role","unexpected","version","reserved","encrypted"};
            ob_puts("control_ignore_"); ob_puts(names[i]); ob_putc(' ');
            ob_dec((int32_t)link.ignore_reasons[i]); ob_puts("\r\n");
        }
        for(i=0;i<4;i++) {
            const ble_update_diag_t *u=&link.update[i];
            ob_puts("conn_update"); ob_ble_field("serial",u->serial); ob_ble_field("epoch",u->epoch);
            ob_ble_field("old",u->old_interval); ob_ble_field("new",u->new_interval);
            ob_ble_field("win",u->win); ob_ble_field("offset",u->offset);
            ob_ble_field("instant",u->instant); ob_ble_field("received",u->received);
            ob_ble_field("outcome",u->outcome); ob_ble_field("reason",u->reason); ob_puts("\r\n");
        }
        ob_kv("acl_tx_accepted",(int32_t)link.acl_in); ob_kv("acl_tx_rejected",(int32_t)link.acl_rejected);
        ob_kv("acl_rx",(int32_t)link.acl_out); ob_kv("acl_acked",(int32_t)link.acknowledged);
        ob_kv("ncp_credits",(int32_t)link.ncp); ob_kv("acl_flushed",(int32_t)link.flushed);
        ob_kv("control_dropped",(int32_t)link.ctrl_drop); ob_kv("control_ignored",(int32_t)link.ignored);
        ob_kv("worker_max_gap_us",(int32_t)(link.worker_gap_ticks/24u));
        ob_kv("worker_max_us",(int32_t)(link.worker_max_ticks/24u));
        ob_kv("worker_over_7500us",(int32_t)link.worker_over_gap);
        ob_kv("worker_calls",(int32_t)link.worker_calls); ob_kv("terminate_timeouts",(int32_t)link.terminate_timeouts);
        ob_kv("nimble_running",host.running); ob_kv("nimble_synced",host.synced);
        ob_kv("nimble_syncs",(int32_t)host.syncs); ob_kv("nimble_resets",(int32_t)host.resets);
        ob_kv("nimble_error",(int32_t)host.error); ob_kv("nimble_assertions",(int32_t)host.assertions);
        ob_kv("nimble_event_drop",(int32_t)host.event_drop); ob_kv("nimble_acl_drop",(int32_t)host.acl_drop);
#if FELUCCA_BLE_STAGE >= 3
        ob_kv("nimble_encrypted",host.encrypted); ob_kv("nimble_bonded",host.bonded); ob_kv("nimble_key_size",host.key_size);
        ob_kv("nimble_pair_method_legacy1_SC2",host.pair_method); ob_kv("security_requests",(int32_t)host.security_requests);
        ob_kv("nimble_bond_sc",host.sc); ob_kv("nimble_bond_identity_known",host.identity_known);
        ob_kv("nimble_peer_identity_type",host.peer_identity_type); ob_puts("nimble_peer_identity "); ob_ble_bytes(host.peer_identity,6);
        ob_kv("security_skipped",(int32_t)host.security_skipped); ob_kv("security_already",(int32_t)host.security_already);
        ob_kv("pair_failures",(int32_t)host.pair_failures);
        ob_kv("pair_last_status",(int32_t)host.pair_status);
#endif
        ob_kv("nimble_main_gap_ms",(int32_t)host.main_gap_ms); ob_kv("nimble_mtu",host.mtu);
        ob_kv("midi_echo",host.echo); ob_kv("nimble_subscribed",host.subscribed); ob_kv("nimble_heap_used",(int32_t)host.heap_used);
        ob_kv("nimble_heap_peak",(int32_t)host.heap_peak); ob_kv("nimble_heap_fail",(int32_t)host.heap_fail);
        ob_kv("midi_packets_in",(int32_t)host.midi_in); ob_kv("midi_packets_out",(int32_t)host.midi_out);
        ob_kv("midi_bad",(int32_t)host.midi_bad); ob_kv("midi_drop",(int32_t)host.midi_drop);
    }
    ob_kv("frozen",ble_console_frozen);
    ob_kv("loop_iters_per_us_x1000",(int32_t)hw.loop_iters_per_us_x1000); ob_kv("loop_ticks",(int32_t)hw.loop_ticks);
    for (i=0;i<4;i++) { ob_puts("clock "); ob_dec((int32_t)i); ob_putc(' '); ob_hex(hw.clocks[i],8); ob_puts("\r\n"); }
    ob_puts("icfg_probe ");
    for (i=0;i<4;i++) { ob_hex(hw.irq_probe[i],8); ob_putc(i==3 ? '\r' : ' '); }
    ob_putc('\n');
    ob_kv("ticks_while_irq_masked",(int32_t)hw.irq_ticks_while_masked);
    ob_kv("error",(int32_t)hw.error); ob_kv("failure_stage",(int32_t)hw.failure_stage); ob_kv("calibration_1_record_2_self",(int32_t)hw.calibration);
    ob_kv("vm_status",ble_cal.vm_status); ob_kv("btif_status",ble_cal.btif_status);
    ob_kx("records_present",ble_cal.present);
    ob_puts("public_address_hci "); ob_ble_bytes(ble_cal.public_addr,6);
    ob_puts("configured_random_hci "); ob_ble_bytes(ble_random_config,6);
    ob_kx("trim_crc",ble_get32(ble_cal.trim+64));
    ob_kv("bringup_us",(int32_t)(hw.bringup_ticks/24));
    ob_kx("bringup_end_ticks",hw.bringup_end_ticks);
    ob_kx("first_hci_command_ticks",d.first_command_ticks);
    for (i=0;i<15;i++) { ob_puts("step_us "); ob_dec((int32_t)i+1); ob_putc(' '); ob_dec((int32_t)(hw.step_ticks[i]/24)); ob_puts("\r\n"); }
    ob_kv("enabled",d.enabled); ob_kv("stopping",d.stopping);
    ob_kv("driver_step",d.driver_step); ob_kv("driver_error",d.driver_error);
    ob_kv("commands",(int32_t)d.commands); ob_kv("replies",(int32_t)d.replies);
    ob_kx("last_opcode",d.last_opcode); ob_kx("last_status",d.last_status);
    ob_kv("intake_rejected",(int32_t)d.rejected); ob_kv("command_overflow",(int32_t)d.overflow);
    ob_kv("advertising_events",(int32_t)hw.adv_events);
    ob_kv("event_irqs",(int32_t)hw.event_irqs); ob_kv("rx_irqs",(int32_t)hw.rx_irqs);
    ob_kv("scan_req",(int32_t)hw.scan_req); ob_kv("connect_ignored",(int32_t)hw.connect_ignored);
    ob_kv("bad_rx",(int32_t)hw.bad_rx);
    ob_kv("rx_max_ticks",(int32_t)hw.rx_max_ticks); ob_kv("event_max_ticks",(int32_t)hw.event_max_ticks);
    ob_kv("rx_over_380us",(int32_t)hw.irq_over_budget);
    ob_kv("event_work_max_ticks",(int32_t)hw.event_work_max_ticks);
    ob_kv("event_work_over_150us",(int32_t)hw.event_over_150us);
    ob_kv("ble_critical_max_ticks",(int32_t)hw.critical_max_ticks);
    ob_kv("ble_critical_over_390us",(int32_t)hw.critical_over_390us);
    ob_kv("main_controller_gap_us",(int32_t)(hw.worker_gap_ticks/24));
    ob_kv("stop_timeouts",(int32_t)hw.stop_timeouts); ob_kv("restarts",(int32_t)hw.restarts);
    ob_kx("enable_ticks",hw.enable_ticks); ob_kx("first_event_close_ticks",hw.first_event_ticks);
    ob_kx("dma_base",hw.dma_base); ob_kx("dma_end",hw.dma_end); ob_kx("modem_base",hw.modem_base);
    ob_kv("dma_canary_errors",(int32_t)hw.canary_errors); ob_kv("p33_timeouts",(int32_t)hw.p33_timeouts);
    ob_kv("trace_records",(int32_t)ble_trace_size()); ob_kv("trace_total",(int32_t)d.trace_count);
    ob_kv("trace_frozen_drops",(int32_t)d.trace_frozen_drops);
    ob_puts("vco_limits "); ob_ble_bytes(hw.vco,48);
}

#pragma clang diagnostic pop

#else
static void open_ble_dump(void) { open_ble_health_dump(); }
#endif
