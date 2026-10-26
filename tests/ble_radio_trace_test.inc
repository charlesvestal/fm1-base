/* SPDX-License-Identifier: MIT OR MPL-2.0 */
static void radio_trace_test(void)
{
    begin();
    for(unsigned e=1;e<=40;e++) {
        sim_ticks+=15u*24000u;
        assert(sim_radio_receive(1,(uint16_t)(1u|(sn<<3)),0,(uint16_t)e)); sn^=1;
        sim_radio_service(0); work(); ble_ll_event((uint16_t)e,sim_ticks);
    }
    direct((uint8_t[]){18},1,3,41,1);
    int reply=opcode(7); assert(reply>=0); sim_ack((unsigned)reply);
    uint8_t ignored[5]={0};
    assert(sim_radio_receive(9,(uint16_t)(0x503u|((sn^1u)<<3)),ignored,42));
    assert(sim_radio_receive(13,0x0513,ignored,43)); sim_radio_service(0); work();
    uint8_t bad[5]={18}; uint64_t counter=next_counter++;
    assert(central_ccm(counter,1,3,bad,1,0)); bad[4]^=1;
    assert(sim_radio_receive(1,(uint16_t)(0x503u|(sn<<3)),bad,44));
    sim_radio_service(0); work(); assert(ble_link.reason==0x3d);
    ble_radio_summary_t summary; ble_radio_summary(&summary);
    assert(summary.frozen && summary.reason==0x3d && summary.last-summary.first+1u==BLE_RADIO_EVENTS);
    static ble_radio_event_t event;
    unsigned empty=0,repeat=0,crc=0,auth_bad=0,ack=0,load=0;
    for(uint32_t serial=summary.first;serial<=summary.last;serial++) {
        assert(ble_radio_read(serial,&event)); assert(!event.overflow);
        for(unsigned i=0;i<event.count;i++) {
            const ble_radio_row_t *r=&event.row[i];
            if(r->kind==BLE_RADIO_ACK) { ack++; assert(r->counter!=BLE_RADIO_NO_COUNTER); }
            if(r->kind==BLE_RADIO_LOAD) { load++; assert(r->counter!=BLE_RADIO_NO_COUNTER); }
            if(r->kind!=BLE_RADIO_RX) continue;
            if(!(r->header>>8)) { empty++; assert(r->counter==BLE_RADIO_NO_COUNTER); }
            if(r->decision==BLE_RADIO_DUPLICATE) { repeat++; assert(r->counter==BLE_RADIO_NO_COUNTER); }
            if(r->decision==BLE_RADIO_CRC) { crc++; assert(r->counter==BLE_RADIO_NO_COUNTER); }
            if(r->decision==BLE_RADIO_AUTH_BAD) { auth_bad++; assert(r->counter==counter && r->slot<2); }
        }
    }
    assert(empty>=BLE_RADIO_EVENTS-4u && repeat==1 && crc==1 && auth_bad==1 && ack==1 && load==1);
    assert(ble_radio_read(summary.last,&event));
    ble_radio_event_t saved=event;
    ble_ll_reset(); begin();
    for(unsigned e=0;e<50;e++) ble_ll_event((uint16_t)e,sim_ticks);
    assert(ble_radio_read(summary.last,&event) && !memcmp(&saved,&event,sizeof saved));
    ble_radio_summary_t after; ble_radio_summary(&after); assert(summary.first==after.first && summary.last==after.last && summary.frozen==after.frozen && summary.reason==after.reason);
    printf("radio ring: %u active events, empty/duplicate/CRC rows, exact auth counter, load/ACK once, frozen through Reset/reconnect: PASS\n",BLE_RADIO_EVENTS);
}

static void radio_timeout_test(void)
{
    begin();
    for(unsigned e=1;e<=40;e++) {
        sim_ticks+=15u*24000u;
        assert(sim_radio_receive(1,(uint16_t)(1u|(sn<<3)),0,(uint16_t)e)); sn^=1;
        sim_radio_service(0); work(); ble_ll_event((uint16_t)e,sim_ticks);
    }
    ble_radio_summary_t before,after; ble_radio_summary(&before);
    for(unsigned e=41;e<600;e++) ble_radio_event((uint16_t)e,1);
    ble_ll_close(8); ble_radio_summary(&after);
    assert(after.frozen && after.reason==8 && after.first==before.first && after.last==before.last);
    printf("central-side MIC/silent timeout: last %u active events retained across 559 silent closes, frozen reason 08: PASS\n",BLE_RADIO_EVENTS);
}
