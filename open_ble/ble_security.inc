/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* Private LL procedures. Included after tx_queue; RX state changes only when
 * the executor consumes a PDU or a marker in the single ordered RX stream. */
#define BLE_CCM_LAST (((uint64_t)1<<39)-1u)
static struct {
    ble_security_diag_t d;
    uint8_t skd[16],iv[8],key[16],ltk[16],request[10];
    uint8_t waiting,ltk_event,reply_pending,rejecting,ever_encrypted,encryption_event,random_pending;
#if FM1B_BLE_DIAG
    ble_mic_pdu_t rx[4],tx[2];
    uint8_t rx_count,tx_count;
    uint64_t prepared_counter;
    uint8_t prepared_length,prepared_encrypted;
#endif
} security __attribute__((section(".pool.ble")));
#if FM1B_BLE_DIAG
/* Separate from session/key state: survives disconnect, reconnect and HCI
 * Reset. Bounded journal retains the last four failures until power-off. */
static ble_mic_record_t mic_records[BLE_MIC_RECORDS] __attribute__((section(".pool.ble")));
static uint32_t mic_serial;
#include "ble_radio_diag.inc"
static void security_history(ble_mic_pdu_t *p,unsigned capacity,uint8_t *count,
                             uint8_t sn,uint64_t counter,uint8_t length)
{
    unsigned n=*count;
    if(n==capacity) { for(unsigned i=1;i<n;i++) p[i-1]=p[i]; n--; }
    p[n].sn=sn; p[n].counter=counter; p[n].length=length; *count=(uint8_t)(n+1u);
}
static void security_mic_record(const rx_t *r,uint64_t counter)
{
    uint32_t serial=++mic_serial;
    ble_mic_record_t *d=&mic_records[(serial-1u)%BLE_MIC_RECORDS];
    memset(d,0,sizeof *d); d->serial=serial; d->ms=ble_port_ms(); d->epoch=r->epoch;
    d->received_ms=r->received_ms; d->since_update_ms=r->since_update_ms;
    d->event=(r->flags&BLE_RX_EVENT_UNKNOWN) ? 0xffff : r->event;
    d->service_event=r->event;
    d->interval=r->interval; d->header=r->header; d->status=r->status;
    d->counter=counter; d->slot=r->slot; d->flags=r->flags;
    d->rx_count=security.rx_count; d->tx_count=security.tx_count;
    memcpy(d->rx,security.rx,sizeof d->rx); memcpy(d->tx,security.tx,sizeof d->tx);
}
unsigned ble_security_mic_records(ble_mic_record_t out[BLE_MIC_RECORDS])
{
    uint32_t key=ble_port_lock(),n=mic_serial<BLE_MIC_RECORDS ? mic_serial : BLE_MIC_RECORDS;
    for(unsigned i=0;i<n;i++) out[i]=mic_records[(mic_serial-n+i)%BLE_MIC_RECORDS];
    ble_port_unlock(key); return (unsigned)n;
}
void ble_security_tx_loaded(unsigned id,uint8_t sn)
{
#if FM1B_BLE_RELIABILITY
    const tx_t *t=&queues.tx[id];
    if(t->radio_counter!=BLE_RADIO_NO_COUNTER)
        security_history(security.tx,2,&security.tx_count,sn,t->radio_counter,(uint8_t)(t->n+4u));
#else
    (void)id;
    if(security.prepared_encrypted)
        security_history(security.tx,2,&security.tx_count,sn,security.prepared_counter,security.prepared_length);
#endif
}
#endif
#if FM1B_BLE_RELIABILITY
#include "ble_reliability.inc"
#endif
static void security_init(void) {
#if FM1B_BLE_RELIABILITY
    if(!reset.active) reliability_cancel();
#endif
    ble_entropy_cancel(BLE_RANDOM_LINK); ble_secure_zero(&security,sizeof security);
}
static void security_new(void)
{
    ble_security_diag_t saved=security.d;
    security_init();
    security.d.mic_failures=saved.mic_failures; security.d.phase_errors=saved.phase_errors;
    security.d.starts=saved.starts; security.d.refreshes=saved.refreshes;
    security.d.ltk_requests=saved.ltk_requests; security.d.negative_replies=saved.negative_replies;
}
static int security_procedure(const uint8_t *p,uint8_t n)
{
    return n && (p[0]==4 || p[0]==5 || p[0]==6 || p[0]==11 || p[0]==13);
}
static int security_tx_allowed(const tx_t *t)
{
    uint8_t op=t->llid==3 && t->n ? t->data[0] : 255;
    if(op==2) return 1;
    switch(security.d.phase) {
    case BLE_ENC_N: return 1;
    case BLE_ENC_S1: return op==4 || op==13 || (!security.rejecting && op==5);
    case BLE_ENC_PA: return op==11;
    default: return 0;
    }
}
static void security_terminate(uint8_t reason)
{
    uint8_t p[2]={2,reason};
    ble_link.local_terminate=1;
    if(tx_queue(p,2,3,1)<0) { ble_ll_close(0x16); return; }
    ble_link.terminate_deadline=ble_port_ticks()+(uint32_t)ble_link.conn.timeout*240000u;
}
static int security_auth_failure(int mic)
{
    if(mic) security.d.mic_failures++; else security.d.phase_errors++;
    ble_ll_close(0x3d); return 0;
}
static void security_marker(uint8_t marker)
{
    switch(marker) {
    case BLE_MARK_RX_ON:
        if(security.d.phase!=BLE_ENC_S1 || security.d.rx_counter) { security_auth_failure(0); return; }
        security.d.rx_on=1; security.d.phase=BLE_ENC_S2; break;
    case BLE_MARK_RX_OFF:
        if(security.d.phase!=BLE_ENC_PA) { security_auth_failure(0); return; }
        security.d.rx_on=0; security.d.rx_counter=0; security.d.phase=BLE_ENC_PB; break;
    case BLE_MARK_REJECT_ACK:
        if(security.rejecting) { security.rejecting=0; security.d.phase=BLE_ENC_N; tx_refill(); }
        break;
    default: security_auth_failure(0); break;
    }
}
static int security_receive_inner(rx_t *r)
{
    unsigned llid=r->header&3u; uint8_t op;
    if(!r->n && llid==1) return 0;
    if(security.d.rx_on && r->n) {
        uint64_t count=security.d.rx_counter++;
#if FM1B_BLE_RELIABILITY
        uint8_t raw[31]; ble_aes_state_t before;
        memcpy(raw,r->data,r->n); ble_port_aes_state(&before);
#endif
        if(count>BLE_CCM_LAST || r->n<5 || !ble_ccm(security.key,security.iv,count,1,(uint8_t)llid,r->data,(uint8_t)(r->n-4u),1)) {
            BLE_DIAG_ONLY(radio_auth(r->radio_id,count,0));
            BLE_DIAG_ONLY(security_mic_record(r,count));
#if FM1B_BLE_RELIABILITY
            reliability_rx_failure(r,count,raw,&before);
#endif
            return security_auth_failure(1);
        }
        BLE_DIAG_ONLY(radio_auth(r->radio_id,count,1));
        BLE_DIAG_ONLY(security_history(security.rx,4,&security.rx_count,(r->header>>3)&1u,count,r->n));
#if FM1B_BLE_RELIABILITY
        reliability.link.rx_ok=1; reliability_inputs_wipe();
#endif
        r->n=(uint8_t)(r->n-4u);
    }
    if(!r->n || !llid) {
        if(security.d.phase!=BLE_ENC_N) return security_auth_failure(0);
        control_ignore(0xff,BLE_IGNORE_RESERVED,r->event); return 0;
    }
    if(security.d.phase==BLE_ENC_N) return 1;
    if(llid!=3) return security_auth_failure(0);
    op=r->data[0];
    if(op==2 && r->n==2) return 1;
    switch(security.d.phase) {
    case BLE_ENC_S1:
        if((op==7 || op==13) && r->n==2) return 1;
        if(op==17 && r->n==3) return 1;
        break;
    case BLE_ENC_S2: if(op==6 && r->n==1) return 1; break;
    case BLE_ENC_PB: if(op==11 && r->n==1) return 1; break;
    case BLE_ENC_PC: if(op==3 && r->n==23) return 1; break;
    default: break;
    }
    return security_auth_failure(0);
}
static int security_receive(rx_t *r)
{
    /* Detection and closing share refill exclusion. In particular CCM's inner
     * AES unlock must not expose a failed MIC to a preempting TX refill. */
    uint32_t key=ble_port_lock(); int accepted=security_receive_inner(r);
#if FM1B_BLE_RELIABILITY
    if(!reliability.failure_active) ble_port_diag_rx_release(r->slot);
#endif
    ble_port_unlock(key); return accepted;
}
static void security_reject(uint8_t error)
{
    uint8_t p[2]={13,error};
    security.waiting=security.ltk_event=0;
    if(security.ever_encrypted) { security_terminate(error); return; }
    security.rejecting=1;
    if(tx_queue(p,2,3,0)<0) ble_ll_close(0x1f);
}
static int security_control(const uint8_t *p,uint8_t n,uint16_t received)
{
    uint8_t reply[13]={0}; uint32_t key;
    if(p[0]==3 && n==23) {
        if(security.d.phase==BLE_ENC_N && security.d.rx_on) { control_ignore(p[0],BLE_IGNORE_ENCRYPTED,received); return 1; }
        if(security.d.phase!=BLE_ENC_N && security.d.phase!=BLE_ENC_PC) return security_auth_failure(0),1;
        security.d.phase=BLE_ENC_S1; security.rejecting=0;
        memcpy(security.request,p+1,10); memcpy(security.skd,p+11,8); memcpy(security.iv,p+19,4);
#if FM1B_BLE_RELIABILITY
        reliability.link.enc_ms=queues.control.received_ms; reliability.link.enc_valid=1;
        reliability.link.ack_valid=0; reliability.link.rx_ok=reliability.link.tx_ok=0;
        reliability.link.old_enc=reliability_old_work();
        memcpy(reliability.link.clear,p+1,10);
        memcpy(reliability.link.skd,p+11,8); memset(reliability.link.skd+8,0,8);
        memcpy(reliability.link.iv,p+19,4); memset(reliability.link.iv+4,0,4);
        ble_secure_zero(reliability.link.ltk,16); ble_secure_zero(reliability.link.skd_input,16);
        reliability.link.inputs=reliability.link.derived_valid=0;
#endif
        security.random_pending=1; ble_port_pend(); return 1;
    }
    if(p[0]==6 && n==1 && security.d.phase==BLE_ENC_S2) {
        key=ble_port_lock();
        security.d.tx_on=security.d.encrypted=1;
        security.d.phase=BLE_ENC_N; reply[0]=6;
        if(tx_queue(reply,1,3,0)<0) ble_ll_close(0x1f);
        else {
            security.encryption_event|=security.ever_encrypted ? 2 : 1;
            if(security.ever_encrypted) security.d.refreshes++; else security.d.starts++;
            security.ever_encrypted=1;
        }
        ble_port_unlock(key); return 1;
    }
    if(p[0]==10 && n==1) {
        if(security.d.phase!=BLE_ENC_N || !security.d.rx_on) { control_ignore(p[0],BLE_IGNORE_UNEXPECTED,received); return 1; }
        security.d.phase=BLE_ENC_PA; reply[0]=11;
        if(tx_queue(reply,1,3,0)<0) ble_ll_close(0x1f);
        return 1;
    }
    if(p[0]==11 && n==1 && security.d.phase==BLE_ENC_PB) {
        key=ble_port_lock(); security.d.tx_on=security.d.encrypted=0; security.d.tx_counter=0;
        security.d.phase=BLE_ENC_PC; ble_port_unlock(key); return 1;
    }
    return 0;
}
uint8_t ble_ll_ltk_reply(uint16_t handle,const uint8_t *ltk,int negative)
{
    if(handle!=BLE_HANDLE || !live()) return 2;
    if(security.d.phase!=BLE_ENC_S1 || !security.waiting || security.reply_pending) return 0x0c;
    security.reply_pending=negative ? 2 : 1; security.waiting=security.ltk_event=0;
    if(!negative) memcpy(security.ltk,ltk,16);
    else security.d.negative_replies++;
    return 0;
}
static void security_worker(void)
{
    uint8_t key_le[16],p=5; unsigned i;
    if(!live() || ble_link.peer_stop) return;
    if(security.random_pending && live()) {
        uint8_t random[12],reply[13]={4};
        int rc=ble_entropy_request(BLE_RANDOM_LINK,random,sizeof random);
        if(!rc) { ble_port_pend(); return; }
        security.random_pending=0;
        if(rc<0) { security_reject(0x1f); return; }
        memcpy(security.skd+8,random,8); memcpy(security.iv+4,random+8,4); memcpy(reply+1,random,12);
#if FM1B_BLE_RELIABILITY
        memcpy(reliability.link.skd,security.skd,16); memcpy(reliability.link.iv,security.iv,8);
#endif
        ble_secure_zero(random,sizeof random);
        if(tx_queue(reply,sizeof reply,3,0)<0) { ble_ll_close(0x1f); return; }
        security.waiting=security.ltk_event=1; security.d.ltk_requests++; return;
    }
    if(!security.reply_pending || !live()) return;
    if(security.reply_pending==2) { security.reply_pending=0; security_reject(6); return; }
    security.reply_pending=0;
    if(!ble_aes_le(security.ltk,security.skd,key_le)) { ble_ll_close(0x1f); return; }
    for(i=0;i<16;i++) security.key[i]=key_le[15u-i];
#if FM1B_BLE_RELIABILITY
    memcpy(reliability.link.ltk,security.ltk,16); memcpy(reliability.link.skd_input,security.skd,16);
    memcpy(reliability.link.derived,security.key,16); memcpy(reliability.link.skd,security.skd,16);
    memcpy(reliability.link.iv,security.iv,8); reliability.link.inputs=reliability.link.derived_valid=1;
#endif
    ble_secure_zero(key_le,sizeof key_le); ble_secure_zero(security.ltk,sizeof security.ltk);
    if(tx_queue(&p,1,3,0)<0) ble_ll_close(0x1f);
}
uint8_t ble_ll_tx_boundary(unsigned id)
{
    tx_t *t=&queues.tx[id];
    if(t->epoch!=ble_link.epoch || t->state!=2 || t->llid!=3 || !t->n) return 0;
    return t->data[0]==5 ? BLE_MARK_RX_ON : t->data[0]==11 ? BLE_MARK_RX_OFF :
        t->data[0]==13 && security.rejecting ? BLE_MARK_REJECT_ACK : 0;
}
int ble_ll_rx_after_marker(uint8_t marker,uint16_t status,uint16_t header,const uint8_t *p)
{
    unsigned n=header>>8;
    if(ble_rx_class(status,(uint8_t)n)!=0 || !n) return 0;
    if(marker==BLE_MARK_RX_ON) return n>=5;
    if(marker==BLE_MARK_RX_OFF) return n<=3;
    return (header&3u)!=3 || (p[0]!=2 && p[0]!=7 && p[0]!=13 && p[0]!=17);
}
int ble_ll_tx_prepare(unsigned id,uint8_t out[31],uint8_t *n)
{
    tx_t *t=&queues.tx[id]; uint64_t count;
    *n=t->n; memcpy(out,t->data,*n);
    BLE_DIAG_ONLY(t->radio_counter=BLE_RADIO_NO_COUNTER; security.prepared_encrypted=0);
    if(!security.d.tx_on || !*n) return 1;
    count=security.d.tx_counter;
    if(count>BLE_CCM_LAST || (count==BLE_CCM_LAST && !t->terminate)) { ble_ll_close(0x16); return 0; }
    BLE_DIAG_ONLY(t->radio_counter=count);
    security.d.tx_counter++;
#if FM1B_BLE_RELIABILITY
    ble_aes_state_t before; ble_port_aes_state(&before);
#endif
    if(!ble_ccm(security.key,security.iv,count,0,t->llid,out,*n,0)) { ble_ll_close(0x1f); return 0; }
    BLE_DIAG_ONLY(security.prepared_counter=count; security.prepared_length=(uint8_t)(*n+4u); security.prepared_encrypted=1);
    *n=(uint8_t)(*n+4u);
#if FM1B_BLE_RELIABILITY
    if(!reliability_tx_check(id,out,*n,count,&before)) return 0;
#endif
    return 1;
}
static int security_poll(uint8_t *p,uint16_t *n)
{
    if(!live()) return 0;
    if(security.ltk_event) {
        p[0]=0x3e; p[1]=13; p[2]=5; ble_put16(p+3,BLE_HANDLE); memcpy(p+5,security.request,10);
        security.ltk_event=0; *n=15; return 1;
    }
    if(security.encryption_event) {
        p[0]=(security.encryption_event&1u) ? 8 : 0x30; p[1]=p[0]==8 ? 4 : 3; p[2]=0;
        ble_put16(p+3,BLE_HANDLE); p[5]=1; *n=p[0]==8 ? 6 : 5; security.encryption_event&=(uint8_t)~(p[0]==8 ? 1u : 2u); return 1;
    }
    return 0;
}
void ble_security_get_diag(ble_security_diag_t *out)
{
    uint32_t key=ble_port_lock(); *out=security.d; ble_port_unlock(key);
}
#ifdef BLE_HOST_TEST
void ble_security_test_counters(uint64_t tx,uint64_t rx) { security.d.tx_counter=tx; security.d.rx_counter=rx; }
#endif
