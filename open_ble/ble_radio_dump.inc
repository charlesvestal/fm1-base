/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Shared USB/CDC formatter. Define BRD_PUTS, BRD_DEC and BRD_HEX. */
static void ble_radio_dump(void)
{
    ble_radio_summary_t s;
    /* Main-context scratch avoids a large stack frame and long IRQ-off
     * sections. Rows contain metadata only. Read a bounded event at a time. */
#ifdef BRD_STACK_EVENT
    ble_radio_event_t e; /* Small Felucca profile: 160 bytes. */
#else
    static ble_radio_event_t e __attribute__((section(".pool.ble")));
#endif
    ble_radio_summary(&s);
    BRD_PUTS("radio_frozen "); BRD_DEC(s.frozen);
    BRD_PUTS(" reason "); BRD_HEX(s.reason,2);
    BRD_PUTS(" events "); BRD_DEC(s.first ? (int32_t)(s.last-s.first+1u) : 0);
    BRD_PUTS("\nradio_tx_source descriptor_only nesn_unknown air_new_repeat_unknown\n");
    if(!s.first) return;
    for(uint32_t serial=s.first;serial<=s.last;serial++) {
        if(!ble_radio_read(serial,&e)) continue;
        BRD_PUTS("radio_event serial "); BRD_DEC((int32_t)e.serial);
        BRD_PUTS(" epoch "); BRD_DEC((int32_t)e.epoch);
        BRD_PUTS(" event "); BRD_DEC(e.event);
        BRD_PUTS(" closed "); BRD_DEC(e.closed);
        BRD_PUTS(" rows "); BRD_DEC(e.count);
        BRD_PUTS(" overflow "); BRD_DEC(e.overflow); BRD_PUTS("\n");
        for(unsigned j=0;j<e.count;j++) {
            const ble_radio_row_t *r=&e.row[j]; int rx=r->kind==BLE_RADIO_RX;
            BRD_PUTS("radio "); BRD_DEC((int32_t)e.serial); BRD_PUTS("/"); BRD_DEC((int32_t)j);
            BRD_PUTS(r->kind==BLE_RADIO_RX ? " rx" : r->kind==BLE_RADIO_LOAD ? " load" : r->kind==BLE_RADIO_ACK ? " ack" : r->kind==BLE_RADIO_PREP ? " prep" : " tx_state");
            BRD_PUTS(" t "); BRD_HEX(r->ticks,8);
            BRD_PUTS(" slot "); BRD_DEC(r->slot); BRD_PUTS(" id "); BRD_DEC(r->id);
            BRD_PUTS(" sn "); BRD_DEC((r->header>>(rx ? 3 : 2))&1u);
            BRD_PUTS(" nesn "); if(rx) BRD_DEC((r->header>>2)&1u); else BRD_PUTS("?");
            BRD_PUTS(" md "); BRD_DEC((r->header>>(rx ? 4 : 3))&1u);
            BRD_PUTS(" llid "); BRD_DEC(r->header&3u);
            BRD_PUTS(" len "); BRD_DEC(r->header>>8);
            BRD_PUTS(" raw "); BRD_HEX(r->status,4);
            BRD_PUTS(" flags "); BRD_HEX(r->flags,2);
            BRD_PUTS(" decision "); BRD_DEC(r->decision);
            BRD_PUTS(" ctr ");
            if(r->counter==BLE_RADIO_NO_COUNTER) BRD_PUTS("none");
            else { BRD_HEX((uint32_t)(r->counter>>32),2); BRD_HEX((uint32_t)r->counter,8); }
            BRD_PUTS("\n");
        }
    }
}
#undef BRD_STACK_EVENT
#undef BRD_PUTS
#undef BRD_DEC
#undef BRD_HEX
