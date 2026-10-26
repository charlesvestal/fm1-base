/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* Reuse the real host/controller/central harness. Only the selected HCI
 * command status is replaced; L2CAP fallback runs through the real host. */
#define main pairing_harness_main
#include "ble_s3_integration.c"
#undef main
static unsigned command_count;
static uint8_t command_error;
int __real_ble_hs_hci_cmd_tx(uint16_t,const void *,uint8_t,void *,uint8_t);
int __wrap_ble_hs_hci_cmd_tx(uint16_t op,const void *cmd,uint8_t len,void *rsp,uint8_t rsp_len)
{
    if(op==0x2013 && command_error) {
        command_count++; return BLE_HS_HCI_ERR(command_error);
    }
    return __real_ble_hs_hci_cmd_tx(op,cmd,len,rsp,rsp_len);
}
int main(void)
{
    struct ble_gap_upd_params params={.itvl_min=12,.itvl_max=12,.latency=0,.supervision_timeout=600};
    sim_reset(); sim_storage_reset(); sim_seed(); sim_executor_enabled=1;
    ble_init(pub,NULL); ble_host_start(); pump(); sim_advance_us(10000); pump(); connect_link();
    struct ble_hs_conn *conn=ble_hs_conn_find(BLE_HANDLE); assert(conn);
    conn->supported_feat|=BLE_HS_HCI_LE_FEAT_CONN_PARAM_REQUEST;
    assert(!(conn->bhc_flags & BLE_HS_CONN_F_MASTER));
    command_error=BLE_ERR_UNSUPP_REM_FEATURE;
    assert(!ble_gap_update_params(BLE_HANDLE,&params)); pump();
    assert(command_count==1 && signaling_len==12 && signaling[0]==0x12 &&
           ble_get16(signaling+2)==8 && ble_get16(signaling+4)==12 &&
           ble_get16(signaling+6)==12 && !ble_get16(signaling+8) && ble_get16(signaling+10)==600);
    uint8_t reject[]={0x13,signaling[1],2,0,1,0}; l2send(5,reject,sizeof reject);
    signaling_len=0;
    command_error=BLE_ERR_INV_HCI_CMD_PARMS;
    assert(ble_gap_update_params(BLE_HANDLE,&params)==BLE_HS_HCI_ERR(command_error)); pump();
    assert(command_count==2 && !signaling_len);
    conn->bhc_flags|=BLE_HS_CONN_F_MASTER; command_error=BLE_ERR_UNSUPP_REM_FEATURE;
    assert(ble_gap_update_params(BLE_HANDLE,&params)==BLE_HS_HCI_ERR(command_error)); pump();
    assert(command_count==3 && !signaling_len);
    conn->bhc_flags&=~BLE_HS_CONN_F_MASTER;
    puts("NimBLE 1.10 GAP: peripheral HCI 0x1a -> real L2CAP update/rejection cleanup; other errors and central role do not fall back: PASS");
    return 0;
}
