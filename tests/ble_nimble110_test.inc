/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* Included in the actual SMP/HCI/LL integration harness. */
static ble_gap_event_fn *application_gap;
static void *application_gap_arg;
static unsigned expect_pair_complete,expected_sc,persisted_completions;
static int observe_pairing(struct ble_gap_event *event,void *arg)
{
    (void)arg;
    if(event->type==BLE_GAP_EVENT_PAIRING_COMPLETE && expect_pair_complete &&
       !event->pairing_complete.status) {
        struct ble_store_key_sec key={0};
        struct ble_store_value_sec value;
        memcpy(key.peer_addr.val,central,6);
        /* This fails on 1.9: persist_keys ran after the application callback. */
        assert(!ble_store_read_our_sec(&key,&value));
        assert(value.ltk_present && value.key_size==16 && value.sc==expected_sc);
        assert(!ble_store_read_peer_sec(&key,&value));
        assert(value.ltk_present && value.key_size==16 && value.sc==expected_sc);
        persisted_completions++; expect_pair_complete=0;
    }
    return application_gap(event,application_gap_arg);
}
static void observe_connection(void)
{
    ble_hs_lock();
    struct ble_hs_conn *conn=ble_hs_conn_find(BLE_HANDLE);
    assert(conn && conn->bhc_cb);
    application_gap=conn->bhc_cb; application_gap_arg=conn->bhc_cb_arg;
    conn->bhc_cb=observe_pairing; conn->bhc_cb_arg=NULL;
    ble_hs_unlock();
}
static void protected_cccd_before_pairing(uint16_t value)
{
    uint8_t q[]={18,0,0,1,0}; ble_put16(q+1,value+1u); l2send(4,q,sizeof q);
    assert(att_n==5 && att[0]==1 && att[1]==18 &&
           ble_get16(att+2)==value+1u && att[4]==BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
    q[0]=10; l2send(4,q,3);
    assert(att_n==3 && att[0]==11 && !ble_get16(att+1));
    ble_host_diag_t d; ble_host_get_diag(&d); assert(!d.subscribed);
}
static void att_110_regressions(uint16_t protected_value)
{
    const char *revision="NimBLE 1.10.0 / open R3";
    uint8_t q[64]={8,1,0,255,255,0x28,0x2a};
    /* A variable-multiple read must check a later forbidden attribute even
     * after an earlier value has filled the initial 23-byte response MTU. */
    uint16_t revision_handle;
    assert(!ble_gatts_find_chr(BLE_UUID16_DECLARE(0x180a),BLE_UUID16_DECLARE(0x2a28),NULL,&revision_handle));
    q[0]=0x20; ble_put16(q+1,revision_handle); ble_put16(q+3,protected_value);
    l2send(4,q,5);
#if MYNEWT_VAL(BLE_VERSION) >= 52
    assert(att_n==5 && att[0]==1 && att[1]==0x20 &&
           ble_get16(att+2)==protected_value && att[4]==BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
#else
    assert(att_n==5 && att[0]==1 && att[1]==0x20 && att[4]==BLE_ATT_ERR_REQ_NOT_SUPPORTED);
#endif
    l2send(4,(uint8_t[]){2,5,2},3); assert(att_n==3 && att[0]==3 && ble_get16(att+1)==517);
    q[0]=8; ble_put16(q+1,1); ble_put16(q+3,0xffff);
    l2send(4,q,7);
    /* Read By Type no longer has a 19-byte temporary value limit. */
    assert(att[0]==9 && att[1]==strlen(revision)+2 && att_n==strlen(revision)+4);
    assert(!memcmp(att+4,revision,strlen(revision)));
    /* Find By Type Value must compare the complete length, not a prefix. */
    q[0]=6; q[5]=0; q[6]=0x2a; memcpy(q+7,"FM-1 Open BLE!",14);
    l2send(4,q,21); assert(att[0]==1 && att[4]==BLE_ATT_ERR_ATTR_NOT_FOUND);
    l2send(4,q,20); assert(att[0]==7 && att_n==5);
}
