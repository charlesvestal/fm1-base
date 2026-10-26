/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* NimBLE host and application: all functions except the two SPSC ring entries
 * execute exclusively in Felucca's main loop, never in a timer/radio ISR. */
#include <string.h>
#include "os/os.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/transport.h"
#include "ble_s1.h"
#include "ble_link.h"
#include "ble_host.h"
#include "ble_midi.h"
#if FELUCCA_BLE_STAGE >= 3
#include "host/ble_sm.h"
#include "ble_bonds.h"
#include "ble_seed.h"
#include "ble_security.h"
#include "ble_crypto.h"
extern int ble_hs_pvcy_remove_entry(uint8_t type,const uint8_t *addr);
#if FELUCCA_BLE_S31
#include "tinycrypt/sha256.h"
extern void ble_hs_pvcy_reset(void); /* pinned host cache, reset at each startup */
#endif
/* Pinned NimBLE internal query; profile has exactly one connection. */
extern int ble_sm_num_procs(void);
#endif
#if FELUCCA_BLE_STAGE >= 3 && !FM1B_BLE_NO_PAIRING
static uint8_t bond_flush_due=1;
static uint8_t gatt_change_inflight;
static ble_addr_t gatt_change_peer;
static uint16_t service_changed_handle;
#endif
#define MIDI_Q 128u
/* A maximum 512-byte attribute can expand to 509 USB program changes. */
#define MIDI_RX_Q 512u
static struct {
    uint32_t pkt[MIDI_RX_Q],ms[MIDI_RX_Q];
    volatile uint32_t w,r;
} midi_rx __attribute__((section(".pool.ble")));
static struct {
    uint32_t pkt[MIDI_Q],ms[MIDI_Q];
    volatile uint32_t w,r;
} midi_tx __attribute__((section(".pool.ble")));
static ble_host_diag_t diag;
#if FM1B_BLE_DIAG
static ble_host_event_t events[BLE_HOST_EVENT_COUNT];
static unsigned events_next,events_count;
static uint32_t event_session;
void ble_host_event(unsigned kind,uint16_t connection,uint32_t a,uint32_t b,uint32_t c)
{
    ble_host_event_t *e=events+events_next;
    if(kind==BLE_HE_CONNECT) event_session++;
    *e=(ble_host_event_t){ble_port_ms(),event_session,a,b,c,connection,(uint16_t)kind};
    events_next=(events_next+1u)%BLE_HOST_EVENT_COUNT;
    if(events_count<BLE_HOST_EVENT_COUNT) events_count++;
}
unsigned ble_host_events(ble_host_event_t out[BLE_HOST_EVENT_COUNT])
{
    for(unsigned i=0;i<events_count;i++)
        out[i]=events[(events_next+BLE_HOST_EVENT_COUNT-events_count+i)%BLE_HOST_EVENT_COUNT];
    return events_count;
}
#endif
static ble_midi_decoder_t decoder;
static ble_midi_usb_t usb_decoder;
static ble_midi_clock_t midi_clock;
#define MIDI_CLOCK_Q 128u
static struct {
    uint32_t pkt[MIDI_CLOCK_Q],due[MIDI_CLOCK_Q];
    uint16_t ts[MIDI_CLOCK_Q];
    volatile uint32_t w,r;
} midi_clock_rx __attribute__((section(".pool.ble")));
static void midi_receive_reset(void)
{
    midi_rx.r=midi_rx.w; midi_clock_rx.r=midi_clock_rx.w;
    memset(&midi_clock,0,sizeof midi_clock);
    memset(&decoder,0,sizeof decoder); memset(&usb_decoder,0,sizeof usb_decoder);
}
static uint16_t midi_handle;
static uint32_t last_main,restart_at,update_at;
static uint8_t update_due;
static uint8_t advertise_pending;
static uint8_t host_enabled=1,advertise_hold,off_reason=BLE_ERR_REM_USER_CONN_TERM;
static const ble_uuid128_t midi_service=BLE_UUID128_INIT(0x00,0xc7,0xc4,0x4e,0xe3,0x6c,0x51,0xa7,0x33,0x4b,0xe8,0xed,0x5a,0x0e,0xb8,0x03);
static const ble_uuid128_t midi_characteristic=BLE_UUID128_INIT(0xf3,0x6b,0x10,0x9d,0x66,0xf2,0xa9,0xa1,0x12,0x41,0x68,0x38,0xdb,0xe5,0x72,0x77);
extern void os_msys_init(void);
extern void os_mempool_module_init(void);
extern void ble_npl_poll(void);
extern void ble_heap_diag(uint32_t *out);
void ble_port_host_idle(void);
static void usb_received(uint32_t pkt,uint16_t ts,void *arg)
{
    uint32_t i=midi_rx.w%MIDI_RX_Q;
    (void)arg;
    if(ble_midi_transport(pkt)) {
        i=midi_clock_rx.w%MIDI_CLOCK_Q;
        if(midi_clock_rx.w-midi_clock_rx.r==MIDI_CLOCK_Q) { diag.midi_drop++; return; }
        midi_clock_rx.pkt[i]=pkt; midi_clock_rx.ts[i]=ts;
        midi_clock_rx.due[i]=ble_midi_release(&midi_clock,pkt,ts,ble_port_ms());
        __asm__ volatile("" ::: "memory"); midi_clock_rx.w++;
        if(diag.echo) ble_host_midi_out(pkt,ts);
        return;
    }
    if(midi_rx.w-midi_rx.r==MIDI_RX_Q) { diag.midi_drop++; return; }
    midi_rx.pkt[i]=pkt; midi_rx.ms[i]=ts;
    __asm__ volatile("" ::: "memory"); midi_rx.w++;
    if(diag.echo) ble_host_midi_out(pkt,ts);
}
static void byte_received(uint8_t byte,uint16_t ts,void *arg) { ble_midi_usb_byte(&usb_decoder,byte,ts,usb_received,arg); }
static int midi_access(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctxt,void *arg)
{
    uint8_t packet[512]; uint16_t n;
    (void)conn; (void)attr; (void)arg;
    if(ctxt->op==BLE_GATT_ACCESS_OP_READ_CHR) {
        ble_host_event(BLE_HE_MIDI_READ,conn,attr,0,0); return 0;
    }
    if(ctxt->op!=BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    n=OS_MBUF_PKTLEN(ctxt->om);
    if(n>sizeof packet || os_mbuf_copydata(ctxt->om,0,n,packet)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if(!ble_midi_decode(&decoder,packet,n,byte_received,0)) { diag.midi_bad++; return BLE_ATT_ERR_UNLIKELY; }
    diag.midi_in++; return 0;
}
#if FELUCCA_BLE_STAGE >= 3 && !FM1B_BLE_NO_PAIRING
#define MIDI_SECURITY (BLE_GATT_CHR_F_READ_ENC|BLE_GATT_CHR_F_WRITE_ENC)
#else
#define MIDI_SECURITY 0
#endif
#ifndef APP_VERSION
#define APP_VERSION "open-r3"
#endif
static int info_access(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctxt,void *arg)
{
    (void)conn; (void)attr;
    if(ctxt->op!=BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    const char *s=arg;
    return os_mbuf_append(ctxt->om,s,(uint16_t)strlen(s)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}
static const struct ble_gatt_chr_def info_chars[]={
    {.uuid=BLE_UUID16_DECLARE(0x2a29),.access_cb=info_access,.arg="M-VAVE",.flags=BLE_GATT_CHR_F_READ},
    {.uuid=BLE_UUID16_DECLARE(0x2a24),.access_cb=info_access,.arg="FM-1",.flags=BLE_GATT_CHR_F_READ},
    {.uuid=BLE_UUID16_DECLARE(0x2a26),.access_cb=info_access,.arg=APP_VERSION,.flags=BLE_GATT_CHR_F_READ},
    {.uuid=BLE_UUID16_DECLARE(0x2a28),.access_cb=info_access,.arg="NimBLE 1.10.0 / open R3",.flags=BLE_GATT_CHR_F_READ},
    {.uuid=0}
};
static const struct ble_gatt_chr_def midi_chars[]={
    { .uuid=&midi_characteristic.u,.access_cb=midi_access,.val_handle=&midi_handle,
      .flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_WRITE_NO_RSP|BLE_GATT_CHR_F_NOTIFY|MIDI_SECURITY|
             (FM1B_BLE_NO_PAIRING ? BLE_GATT_CHR_F_WRITE : 0) },
    { .uuid=0 }
};
static const struct ble_gatt_svc_def services[]={
    { .type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&midi_service.u,.characteristics=midi_chars },
    /* Append services: keep R1 GAP, GATT / Service Changed, and MIDI handles. */
    { .type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=BLE_UUID16_DECLARE(0x180a),.characteristics=info_chars }, { .type=0 }
};
static int gap_event(struct ble_gap_event *event,void *arg)
{
    (void)arg;
    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if(!event->connect.status) { midi_receive_reset(); diag.connection=event->connect.conn_handle; diag.mtu=23; update_due=1; update_at=ble_port_ms()+5000u; }
        else { advertise_pending=1; restart_at=ble_port_ms()+100u; }
#if FELUCCA_BLE_STAGE >= 3 && !FM1B_BLE_NO_PAIRING
        if(!event->connect.status) { gatt_change_inflight=0; diag.gatt_peer_version=0; diag.pair_method=diag.sc=0; }
#endif
        if(!event->connect.status) {
            struct ble_gap_conn_desc desc;
            if(!ble_gap_conn_find(diag.connection,&desc))
                ble_host_event(BLE_HE_CONNECT,diag.connection,desc.peer_ota_addr.type,
                    ble_addr_cmp(&desc.peer_id_addr,&desc.peer_ota_addr)!=0,desc.peer_id_addr.type);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ble_host_event(BLE_HE_DISCONNECT,diag.connection,(uint32_t)event->disconnect.reason&255u,
            (uint32_t)event->disconnect.reason,0);
#if FELUCCA_BLE_STAGE >= 3 && !FM1B_BLE_NO_PAIRING
        gatt_change_inflight=0; bond_flush_due=1; diag.encrypted=diag.bonded=diag.key_size=0;
#endif
        diag.connection=BLE_HS_CONN_HANDLE_NONE; diag.subscribed=0; update_due=0;
        midi_receive_reset();
        midi_tx.r=midi_tx.w; advertise_pending=1; restart_at=ble_port_ms()+100u; break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if(event->subscribe.attr_handle==midi_handle) diag.subscribed=event->subscribe.cur_notify;
        if(event->subscribe.reason==BLE_GAP_SUBSCRIBE_REASON_RESTORE)
            ble_host_event(BLE_HE_CCCD_RESTORE,event->subscribe.conn_handle,
                event->subscribe.attr_handle+1u,event->subscribe.cur_notify |
                (event->subscribe.cur_indicate<<1),0);
        break;
    case BLE_GAP_EVENT_MTU:
        diag.mtu=event->mtu.value;
        ble_host_event(BLE_HE_MTU,event->mtu.conn_handle,event->mtu.value,0,0); break;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        diag.param_status=(uint32_t)event->conn_update.status;
        if(!event->conn_update.status && !ble_gap_conn_find(event->conn_update.conn_handle,&desc))
            ble_host_event(BLE_HE_CONN_UPDATE,event->conn_update.conn_handle,
                desc.conn_itvl,desc.conn_latency,desc.supervision_timeout);
        break;
    }
#if FELUCCA_BLE_STAGE >= 3 && !FM1B_BLE_NO_PAIRING
    case BLE_GAP_EVENT_NOTIFY_TX:
        if(event->notify_tx.indication && event->notify_tx.attr_handle==service_changed_handle &&
           event->notify_tx.conn_handle==diag.connection && gatt_change_inflight &&
           event->notify_tx.status==BLE_HS_EDONE) {
            gatt_change_inflight=0;
            if(!ble_bonds_db_confirm(gatt_change_peer.type,gatt_change_peer.val,BLE_GATT_DB_VERSION)) {
                diag.gatt_peer_version=BLE_GATT_DB_VERSION; bond_flush_due=1;
                diag.gatt_confirmations++;
            }
            ble_host_event(BLE_HE_SC_CONFIRMED,diag.connection,BLE_GATT_DB_VERSION,0,0);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        if(event->enc_change.status) diag.pair_failures++;
        diag.pair_status=(uint32_t)event->enc_change.status;
        if(!ble_gap_conn_find(event->enc_change.conn_handle,&desc)) {
            diag.encrypted=desc.sec_state.encrypted; diag.bonded=desc.sec_state.bonded;
            diag.key_size=desc.sec_state.key_size;
#if FM1B_BLE_OPEN
            if(!event->enc_change.status && desc.sec_state.encrypted) {
                ble_bonds_touch(desc.peer_id_addr.type,desc.peer_id_addr.val); bond_flush_due=1;
            }
#endif
        }
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if(ble_gap_conn_find(event->repeat_pairing.conn_handle,&desc)) return BLE_GAP_REPEAT_PAIRING_IGNORE;
        int rc=ble_host_delete_bond(desc.peer_id_addr.type,desc.peer_id_addr.val);
        if(rc) { diag.error=(uint32_t)rc; return BLE_GAP_REPEAT_PAIRING_IGNORE; }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
#endif
    default: break;
    }
    return 0;
}
static void advertise(void)
{
    struct ble_gap_adv_params params;
    static const uint8_t adv[]={2,1,6,17,7,0x00,0xc7,0xc4,0x4e,0xe3,0x6c,0x51,0xa7,0x33,0x4b,0xe8,0xed,0x5a,0x0e,0xb8,0x03};
    uint8_t scan[31]; const char *name=ble_svc_gap_device_name();
    unsigned name_len=(unsigned)strlen(name),n=name_len>29u ? 29u : name_len;
    scan[0]=(uint8_t)(n+1u); scan[1]=name_len>29u ? 8u : 9u; memcpy(scan+2,name,n);
    int rc=ble_gap_adv_set_data(adv,sizeof adv);
    if(!rc) rc=ble_gap_adv_rsp_set_data(scan,(int)(n+2u));
    memset(&params,0,sizeof params); params.conn_mode=BLE_GAP_CONN_MODE_UND;
    params.disc_mode=BLE_GAP_DISC_MODE_GEN; params.itvl_min=params.itvl_max=32; /* 20 ms, including every restart */
    if(!rc) rc=ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC,0,BLE_HS_FOREVER,&params,gap_event,0);
    if(rc) { diag.error=(uint32_t)rc; restart_at=ble_port_ms()+1000u; }
    else advertise_pending=0;
}
static void synced(void)
{
#if FELUCCA_BLE_S31
    /* Pinned HCI-4.0 startup mask 001F plus Enhanced Connection Complete.
     * This callback runs after every host startup, before advertising. */
    static const uint8_t mask[8]={0x1f,2,0,0,0,0,0,0};
    extern int ble_hs_hci_cmd_tx(uint16_t,const void *,uint8_t,void *,uint8_t);
    int rc=ble_hs_hci_cmd_tx(0x2001,mask,sizeof mask,0,0);
    if(rc) { diag.error=(uint32_t)rc; advertise_pending=0; ble_hs_sched_reset(rc); return; }
#endif
    /* Keep this handle stable for the lifetime of the identity. Cache changes
     * are per bond; never fan out an update to other peers via chr_updated. */
#if FELUCCA_BLE_STAGE >= 3 && !FM1B_BLE_NO_PAIRING
    if(ble_gatts_find_chr(BLE_UUID16_DECLARE(0x1801),BLE_UUID16_DECLARE(0x2a05),0,&service_changed_handle)) {
        diag.error=BLE_HS_ENOENT; return;
    }
#endif
    diag.error=0; diag.synced=1; diag.syncs++; advertise_pending=1; restart_at=ble_port_ms();
}
static void reset(int reason) { diag.resets++; diag.error=(uint32_t)reason; diag.synced=diag.subscribed=0; }
static int gen_key(uint8_t key,struct ble_store_gen_key *out,uint16_t conn)
{
    (void)conn;
    if(key!=BLE_STORE_GEN_KEY_IRK) return BLE_HS_ENOTSUP;
    memset(out,0,sizeof *out);
#if FELUCCA_BLE_S31
    {
        uint8_t address[6];
        if(ble_hs_id_copy_addr(BLE_ADDR_PUBLIC,address,0)) return BLE_HS_ENOADDR;
        ble_host_local_irk(address,out->irk);
        /* Keep initial IRK installation deterministic too. NimBLE 1.10 also
         * clears this cache itself on the HCI error/reset path. */
        ble_hs_pvcy_reset();
    }
#endif
    return 0;
}
#if FELUCCA_BLE_S31
void ble_host_local_irk(const uint8_t address[6],uint8_t irk[16])
{
    static const uint8_t domain[]="Felucca S3.1 public identity IRK v1";
    struct tc_sha256_state_struct s; uint8_t digest[32];
    tc_sha256_init(&s); tc_sha256_update(&s,domain,sizeof domain-1u);
    tc_sha256_update(&s,address,6); tc_sha256_final(digest,&s);
    memcpy(irk,digest,16); irk[0]|=1; ble_secure_zero(digest,sizeof digest);
}
#endif
/* All application peer deletions, including overflow replacement, use this
 * path. ble_gap_unpair would terminate the live repeat-pairing connection. */
#if FELUCCA_BLE_STAGE >= 3 && !FM1B_BLE_NO_PAIRING
int ble_host_delete_bond(uint8_t identity_type,const uint8_t identity[6])
{
    ble_addr_t addr; int rc;
    if(identity_type>BLE_ADDR_RANDOM_ID) return BLE_HS_EINVAL;
    addr.type=identity_type&1u; memcpy(addr.val,identity,6);
#if FELUCCA_BLE_S31
    rc=ble_hs_pvcy_remove_entry(addr.type,addr.val);
    if(rc && rc!=BLE_HS_ERR_HCI_BASE+BLE_ERR_UNK_CONN_ID) return rc;
#endif
    rc=ble_store_util_delete_peer(&addr);
    if(!rc) bond_flush_due=1;
    return rc;
}
static int store_read(int type,const union ble_store_key *key,union ble_store_value *value)
{
    int rc=ble_bonds_read(type,key,value);
    /* R2 may have left a global pending flag in a disconnected peer's CCCD.
     * Suppress NimBLE's automatic replay (its range is unset after reboot).
     * The per-bond generation below is the sole authority for cache changes. */
    if(!rc && type==BLE_STORE_OBJ_TYPE_CCCD && value->cccd.chr_val_handle==service_changed_handle)
        value->cccd.value_changed=0;
    return rc;
}
static int store_write(int type,const union ble_store_value *value)
{
#if FELUCCA_BLE_S31
    if(type==BLE_STORE_OBJ_TYPE_OUR_SEC && value->sec.ltk_present) {
        union ble_store_key key; union ble_store_value old; int rc;
        memset(&key,0,sizeof key); key.sec.peer_addr=value->sec.peer_addr;
        /* A new IRK may make the connection unresolved until SMP identity
         * distribution. In that case NimBLE cannot issue REPEAT_PAIRING at
         * Pairing Request. It persists OUR_SEC first: replace the old bond
         * here, before either new security direction is written. */
        rc=ble_bonds_read(type,&key,&old);
        if(!rc && old.sec.ltk_present && (old.sec.sc!=value->sec.sc ||
           old.sec.ediv!=value->sec.ediv || old.sec.rand_num!=value->sec.rand_num ||
           memcmp(old.sec.ltk,value->sec.ltk,16))) {
            rc=ble_host_delete_bond(value->sec.peer_addr.type,value->sec.peer_addr.val);
            if(rc) return rc;
        } else if(rc && rc!=BLE_HS_ENOENT) return rc;
    }
#endif
    return ble_bonds_write(type,value);
}
static int store_status(struct ble_store_status_event *event,void *arg)
{
#if FM1B_BLE_OPEN
    ble_bond_diag_t d; ble_bond_peer_diag_t peer; struct ble_gap_conn_desc conn;
    const ble_addr_t *incoming;
    (void)arg;
    /* FULL is a pessimistic SMP preflight (even at N-1); wait for an actual
     * overflow so failed pairings and rewrites never evict a retained bond. */
    if(event->event_code==BLE_STORE_EVENT_FULL) return 0;
    if(event->event_code!=BLE_STORE_EVENT_OVERFLOW) return BLE_HS_EUNKNOWN;
    int type=event->overflow.obj_type;
    if(type<1 || type>3) return BLE_HS_EUNKNOWN;
    incoming=type==3 ? &event->overflow.value->cccd.peer_addr : &event->overflow.value->sec.peer_addr;
    ble_bonds_get_diag(&d);
    /* An existing peer already owns its two CCCD slots; deleting some other
     * peer cannot make a third CCCD fit. Do not cascade replacements. */
    for(unsigned n=0;n<d.count;n++) if(!ble_bonds_peer(n,&peer) &&
        (incoming->type&1u)==peer.type && !memcmp(incoming->val,peer.addr,6)) return BLE_HS_ESTORE_CAP;
    for(unsigned n=d.count;n>0;n--) if(!ble_bonds_peer(n-1u,&peer)) {
        ble_addr_t addr; addr.type=peer.type; memcpy(addr.val,peer.addr,6);
        if(!ble_gap_conn_find_by_addr(&addr,&conn)) continue;
        int rc=ble_host_delete_bond(addr.type,addr.val);
        if(!rc) ble_bonds_replaced();
        return rc;
    }
    return BLE_HS_ENOMEM;
#else
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)]; int count,rc,i;
    (void)arg;
    if(event->event_code==BLE_STORE_EVENT_FULL) return 0;
    if(event->event_code!=BLE_STORE_EVENT_OVERFLOW) return BLE_HS_EUNKNOWN;
    if(event->overflow.obj_type!=BLE_STORE_OBJ_TYPE_OUR_SEC &&
       event->overflow.obj_type!=BLE_STORE_OBJ_TYPE_PEER_SEC &&
       event->overflow.obj_type!=BLE_STORE_OBJ_TYPE_CCCD) return BLE_HS_EUNKNOWN;
    rc=ble_store_util_bonded_peers(peers,&count,MYNEWT_VAL(BLE_STORE_MAX_BONDS));
    if(rc) return rc;
    for(i=0;i<count;i++) {
        if(event->overflow.obj_type==BLE_STORE_OBJ_TYPE_CCCD &&
           !ble_addr_cmp(peers+i,&event->overflow.value->cccd.peer_addr)) continue;
        return ble_host_delete_bond(peers[i].type,peers[i].val);
    }
    return BLE_HS_ENOMEM;
#endif
}
#endif
static void receive_main(uint8_t type,const uint8_t *packet,uint16_t n)
{
    if(type==4) {
        if((n>=6 && packet[0]==8) || (n>=5 && packet[0]==0x30))
            ble_host_event(packet[0]==8 ? BLE_HE_ENC_START : BLE_HE_ENC_REFRESH,
                ble_get16(packet+3),packet[2],packet[0]==8 ? packet[5] : 1,0);
        void *event=ble_transport_alloc_evt(0);
        if(!event) { diag.event_drop++; return; }
        memcpy(event,packet,n); ble_transport_to_hs_evt(event);
    } else if(type==2) {
        struct os_mbuf *om=ble_transport_alloc_acl_from_ll();
        if(!om || os_mbuf_append(om,packet,n)) { if(om) os_mbuf_free_chain(om); diag.acl_drop++; return; }
        ble_transport_to_hs_acl(om);
    }
}
/* r4 04 §10.5 alternative: the executor copies into this bounded ring.
 * Only main (including the synchronous ACK wait) enters NimBLE's pools. */
static struct { uint8_t type,n,p[70]; } host_ring[32] __attribute__((section(".ble_host_ring")));
static uint8_t host_r,host_w,host_count;
static void receive(uint8_t type,const uint8_t *packet,uint16_t n)
{
    uint32_t key=ble_port_lock();
    if(n>70 || host_count==32) { if(type==2) diag.acl_drop++; else diag.event_drop++; }
    else {
        host_ring[host_w].type=type; host_ring[host_w].n=(uint8_t)n;
        memcpy(host_ring[host_w].p,packet,n); host_w=(uint8_t)((host_w+1u)%32u); host_count++;
    }
    ble_port_unlock(key);
}
void ble_host_transport_poll(void)
{
    unsigned budget=32;
    while(budget--) {
        uint32_t key=ble_port_lock(); uint8_t type,n,p[70];
        if(!host_count) { ble_port_unlock(key); break; }
        type=host_ring[host_r].type; n=host_ring[host_r].n; memcpy(p,host_ring[host_r].p,n);
        host_r=(uint8_t)((host_r+1u)%32u); host_count--; ble_port_unlock(key);
        receive_main(type,p,n);
    }
}
void ble_transport_ll_init(void) { }
int ble_transport_to_ll_cmd_impl(void *buf)
{
    uint8_t *p=buf; int rc=ble_hci_send(1,p,(uint16_t)(p[2]+3u));
    ble_transport_free(buf); return rc ? BLE_HS_ECONTROLLER : 0;
}
int ble_transport_to_ll_acl_impl(struct os_mbuf *om)
{
    uint8_t packet[31]; uint16_t n=OS_MBUF_PKTLEN(om); int rc=1;
    if(n<=sizeof packet && !os_mbuf_copydata(om,0,n,packet)) rc=ble_hci_send(2,packet,n);
    os_mbuf_free_chain(om); return rc ? BLE_HS_EAGAIN : 0;
}
int ble_transport_to_ll_iso_impl(struct os_mbuf *om) { os_mbuf_free_chain(om); return BLE_HS_ENOTSUP; }
void ble_host_wait_step(void) { ble_host_transport_poll(); ble_port_host_idle(); }
void ble_host_start(void)
{
    int rc;
    ble_set_host(receive); os_mempool_module_init(); os_msys_init();
    ble_transport_init(); ble_hs_init();
    ble_hs_cfg.sync_cb=synced; ble_hs_cfg.reset_cb=reset; ble_hs_cfg.store_gen_key_cb=gen_key;
#if FELUCCA_BLE_STAGE >= 3 && !FM1B_BLE_NO_PAIRING
    ble_bonds_init();
    ble_hs_cfg.sm_io_cap=BLE_SM_IO_CAP_NO_IO; ble_hs_cfg.sm_bonding=1; ble_hs_cfg.sm_mitm=0; ble_hs_cfg.sm_sc=1;
    ble_hs_cfg.sm_our_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_read_cb=store_read; ble_hs_cfg.store_write_cb=store_write;
    ble_hs_cfg.store_delete_cb=ble_bonds_delete; ble_hs_cfg.store_status_cb=store_status;
#endif
    ble_svc_gap_init(); ble_svc_gatt_init();
    rc=ble_gatts_count_cfg(services); if(!rc) rc=ble_gatts_add_svcs(services);
    diag.connection=BLE_HS_CONN_HANDLE_NONE;
    if(rc) diag.error=(uint32_t)rc; else diag.running=1;
}
void ble_host_worker(void)
{
    uint32_t now=ble_port_ms(),gap=now-last_main; unsigned budget=32;
    if(last_main && gap>diag.main_gap_ms) diag.main_gap_ms=gap;
    last_main=now;
    if(!diag.running) return;
    ble_npl_poll(); ble_host_transport_poll();
    while(budget--) {
        struct ble_npl_event *event=ble_npl_eventq_get(nimble_port_get_dflt_eventq(),0);
        if(!event) break;
        ble_npl_event_run(event); ble_host_transport_poll();
    }
    if(!host_enabled && diag.connection!=BLE_HS_CONN_HANDLE_NONE)
        (void)ble_gap_terminate(diag.connection,off_reason);
#if FELUCCA_BLE_STAGE >= 3 && !FM1B_BLE_NO_PAIRING
    if(bond_flush_due && diag.connection==BLE_HS_CONN_HANDLE_NONE && ble_port_storage_idle()) {
        ble_bond_diag_t bd;
        ble_bonds_flush(); ble_bonds_get_diag(&bd); bond_flush_due=bd.pending;
    }
    if(host_enabled && !gatt_change_inflight && diag.encrypted && diag.bonded &&
       diag.connection!=BLE_HS_CONN_HANDLE_NONE && !ble_sm_num_procs()) {
        struct ble_gap_conn_desc desc; uint32_t version; uint8_t flags=0;
        if(!ble_gap_conn_find(diag.connection,&desc) &&
           !ble_bonds_db_version(desc.peer_id_addr.type,desc.peer_id_addr.val,&version)) {
            diag.gatt_peer_version=version;
            if(version!=BLE_GATT_DB_VERSION &&
               !ble_gatts_read_cccd(diag.connection,service_changed_handle,&flags) && (flags&2u)) {
                static const uint8_t range[]={1,0,255,255};
                struct os_mbuf *om=ble_hs_mbuf_from_flat(range,sizeof range);
                if(om) {
                    gatt_change_peer=desc.peer_id_addr;
                    int rc=ble_gatts_indicate_custom(diag.connection,service_changed_handle,om);
                    if(!rc) {
                        gatt_change_inflight=1; diag.gatt_invalidations++;
                        ble_host_event(BLE_HE_SC_SENT,diag.connection,1,0xffff,version);
                    }
                }
            }
        }
    }
#endif
    if(host_enabled && !advertise_hold && !ble_host_linked() && diag.synced && advertise_pending && (int32_t)(now-restart_at)>=0) advertise();
    if(host_enabled && update_due && (int32_t)(now-update_at)>=0 && diag.connection!=BLE_HS_CONN_HANDLE_NONE
#if FELUCCA_BLE_STAGE >= 3 && !FM1B_BLE_NO_PAIRING
       && !ble_sm_num_procs()
#endif
    ) {
        struct ble_gap_upd_params params;
        memset(&params,0,sizeof params); params.itvl_min=params.itvl_max=12; /* MIDI: 15 ms, latency zero */
        params.supervision_timeout=600; /* Apple ADG 2026-09-21: >=6 seconds */
        /* Negotiated octet 0 has no connection-parameter-request bit: NimBLE
         * sends an L2CAP signalling request, never peripheral HCI 2013. */
        diag.param_requests++;
        diag.param_status=(uint32_t)ble_gap_update_params(diag.connection,&params);
        ble_host_event(BLE_HE_PARAM_SENT,diag.connection,12,600,diag.param_status);
        if(diag.param_status==BLE_HS_EBUSY || diag.param_status==BLE_HS_EALREADY) update_at=now+1000u;
        else update_due=0;
    }
    budget=16;
    while(budget-- && midi_tx.r!=midi_tx.w) {
        uint32_t i=midi_tx.r%MIDI_Q; uint8_t packet[8]; int n,rc;
        struct os_mbuf *om;
        if(!host_enabled || !diag.subscribed) { midi_tx.r=midi_tx.w; break; }
#if FELUCCA_BLE_STAGE >= 3 && !FM1B_BLE_NO_PAIRING
        if(!diag.encrypted) break;
#endif
        n=ble_midi_encode_usb(midi_tx.pkt[i],midi_tx.ms[i],packet,sizeof packet);
        if(!n) { midi_tx.r++; diag.midi_bad++; continue; }
        om=ble_hs_mbuf_from_flat(packet,(uint16_t)n);
        if(!om) break;
        rc=ble_gatts_notify_custom(diag.connection,midi_handle,om);
        if(rc==BLE_HS_ENOMEM || rc==BLE_HS_EAGAIN) break;
        midi_tx.r++; if(rc) diag.midi_drop++; else diag.midi_out++;
    }
}
void ble_host_midi_out(uint32_t pkt,uint32_t ms)
{
    uint32_t key=ble_port_lock(),i=midi_tx.w%MIDI_Q;
    if(!host_enabled || !diag.subscribed) { ble_port_unlock(key); return; }
    if(midi_tx.w-midi_tx.r==MIDI_Q) { diag.midi_drop++; ble_port_unlock(key); return; }
    midi_tx.pkt[i]=pkt; midi_tx.ms[i]=ms;
    __asm__ volatile("" ::: "memory"); midi_tx.w++; ble_port_unlock(key);
}
void ble_host_echo(int enabled) { diag.echo=(uint8_t)!!enabled; }

int ble_host_midi_take(uint32_t *pkt,uint32_t *ms)
{
    uint32_t i=midi_rx.r%MIDI_RX_Q;
    /* Due transport first; an undued clock never blocks notes or SysEx. */
    uint32_t at=midi_clock_rx.r%MIDI_CLOCK_Q;
    if(midi_clock_rx.r!=midi_clock_rx.w && (int32_t)(ble_port_ms()-midi_clock_rx.due[at])>=0) {
        *pkt=midi_clock_rx.pkt[at]; *ms=midi_clock_rx.ts[at]; midi_clock_rx.r++; return 1;
    }
    if(midi_rx.r==midi_rx.w) return 0;
    *pkt=midi_rx.pkt[i]; *ms=midi_rx.ms[i]; midi_rx.r++; return 1;
}
void ble_host_clock_read(ble_midi_clock_diag_t *out) { ble_midi_clock_read(&midi_clock,out); }
void ble_host_get_diag(ble_host_diag_t *out)
{
    uint32_t heap[3]; ble_heap_diag(heap); *out=diag; out->param_pending=update_due;
#if FELUCCA_BLE_STAGE >= 3 && !FM1B_BLE_NO_PAIRING
    /* ENC_CHANGE precedes persistence. Query afresh, never another peer's
     * last write; unknown/unbonded links explicitly report method zero. */
    {
        struct ble_gap_conn_desc desc; union ble_store_key key; union ble_store_value value;
        out->pair_method=out->sc=out->identity_known=0;
        if(diag.connection!=BLE_HS_CONN_HANDLE_NONE && !ble_gap_conn_find(diag.connection,&desc)) {
            out->peer_identity_type=desc.peer_id_addr.type; memcpy(out->peer_identity,desc.peer_id_addr.val,6);
            memset(&key,0,sizeof key); key.sec.peer_addr=desc.peer_id_addr;
            if(!ble_bonds_read(1,&key,&value)) {
                out->pair_method=value.sec.sc ? 2 : 1; out->sc=value.sec.sc; out->identity_known=1;
            }
        }
    }
#endif
    out->heap_used=heap[0]; out->heap_peak=heap[1]; out->heap_fail=heap[2];
}
void ble_host_assert(const char *file,int line)
{
    (void)file; diag.assertions++; diag.error=(uint32_t)line; diag.running=0;
}

void ble_host_manual(void)
{
    diag.running=0; diag.subscribed=0; ble_set_host(0); midi_tx.r=midi_tx.w;
}
void ble_host_resume(void)
{
    ble_set_host(receive); diag.running=1; advertise_pending=0;
    ble_hs_sched_reset(BLE_HS_ECONTROLLER);
}

/* Application requests run in main, including after a rapid off/on cycle. */
void ble_host_enable(int on)
{
    host_enabled=(uint8_t)!!on;
    if(!host_enabled) {
        advertise_pending=update_due=0;
        if(ble_gap_adv_active()) (void)ble_gap_adv_stop();
        if(diag.connection!=BLE_HS_CONN_HANDLE_NONE)
            (void)ble_gap_terminate(diag.connection,off_reason);
        midi_tx.r=midi_tx.w;
    } else { advertise_pending=1; restart_at=ble_port_ms()+100u; }
}
/* The reason a disable gives its link (Core Vol 1 Part F): 0x13 Remote User Terminated Connection (the default:
 * Bluetooth switched off, a restore), 0x15 Remote Device Terminated Connection due to Power Off (an update: the FM-1
 * restarts into its loader). Others are kept to these two; the next disable uses it. */
void ble_host_off_reason(uint8_t reason)
{
    off_reason=reason==BLE_ERR_RD_CONN_TERM_PWROFF ? BLE_ERR_RD_CONN_TERM_PWROFF : BLE_ERR_REM_USER_CONN_TERM;
}
uint32_t ble_host_midi_room(void)
{
    uint32_t k=ble_port_lock();
    uint32_t room=host_enabled && diag.subscribed ? MIDI_Q-(midi_tx.w-midi_tx.r) : 0;
    ble_port_unlock(k); return room;
}
int ble_host_linked(void) { return diag.connection!=BLE_HS_CONN_HANDLE_NONE; }
int ble_host_connected(void) { return host_enabled && ble_host_linked() && diag.subscribed; }

/* Main-context flash window. A live or newly accepted link is never stopped
 * just to save app data. HCI stop completes before the platform write guard. */
int ble_host_pause_advertising(void)
{
    if(ble_host_linked()) return -8;
    advertise_hold=1;
    if(ble_gap_adv_active() && ble_gap_adv_stop()) { advertise_hold=0; return -8; }
    return 0;
}
void ble_host_resume_advertising(void)
{
    advertise_hold=0;
    if(host_enabled && !ble_host_linked()) { advertise_pending=1; restart_at=ble_port_ms()+100u; }
}
