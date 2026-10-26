/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
/* F-29: actual NimBLE SMP, controller HCI/CCM, object A/B NOR, no devices. */
#include "../open_ble/ble_store.h"
extern unsigned writes,erases;
static uint8_t capacity_ltk[10][16],capacity_rand[10][10];
static void select_peer(unsigned id,unsigned session)
{
    memset(central,0,6); central[0]=(uint8_t)id;
    memcpy(peer_irk,sample_irk,16); peer_irk[0]^=(uint8_t)(id-1u);
    current_rpa[3]=(uint8_t)id; current_rpa[4]=(uint8_t)session; current_rpa[5]=0x70;
    assert(ble_ah(peer_irk,current_rpa+3,current_rpa)); reconnect_rpa=1;
}
static void stored(unsigned id,int present)
{
    union ble_store_key k={0}; union ble_store_value v;
    k.sec.peer_addr.val[0]=(uint8_t)id;
    assert(ble_bonds_read(1,&k,&v)==(present ? 0 : BLE_HS_ENOENT));
    if(present) {
        assert(!memcmp(v.sec.ltk,capacity_ltk[id],16));
        uint32_t version; assert(!ble_bonds_db_version(0,k.sec.peer_addr.val,&version) && version==3);
        memset(&k,0,sizeof k); k.cccd.peer_addr.val[0]=(uint8_t)id;
        assert(!ble_bonds_read(3,&k,&v) && v.cccd.flags);
        k.cccd.idx=1;
        int rc=ble_bonds_read(3,&k,&v);
        if(rc || !v.cccd.flags) { ble_bond_peer_diag_t p; for(unsigned n=0;!ble_bonds_peer(n,&p);n++) fprintf(stderr,"id %u rank %u cccd %u=%u %u=%u\n",p.addr[0],p.rank,p.cccd_handle[0],p.cccd_flags[0],p.cccd_handle[1],p.cccd_flags[1]); fprintf(stderr,"missing second CCCD id %u rc %d\n",id,rc); }
        assert(!rc && v.cccd.flags);
        k.cccd.idx=2; assert(ble_bonds_read(3,&k,&v)==BLE_HS_ENOENT);
    }
}
static void capacity_reboot(void)
{
    ble_object_init(); ble_bonds_init();
    ble_host_resume(); pump(); sim_advance_us(101000); pump(); privacy_ready(9);
}
static void capacity_reconnect(unsigned id,unsigned session)
{
    select_peer(id,session); sim_advance_us(101000); pump(); connect_link();
    assert(ble_link.peer_resolved && !memcmp(ble_link.peer_identity,central,6));
    encryption_start(capacity_ltk[id],capacity_rand[id]);
    ble_host_diag_t d; ble_host_get_diag(&d); assert(d.encrypted && d.bonded && d.subscribed);
    disconnect_link(); stored(id,1);
}
static void capacity_test(void)
{
    static const unsigned order[8]={4,2,8,1,6,3,7,5};
    ble_host_diag_t h; ble_bond_diag_t d; ble_bond_peer_diag_t peer;
    sim_reset(); sim_storage_reset(); sim_seed(); sim_executor_enabled=1;
    ble_init(pub,0); ble_host_start(); pump(); sim_advance_us(10000); pump(); privacy_ready(1);
    uint16_t midi=0;
    for(unsigned id=1;id<=8;id++) {
        select_peer(id,0); sim_advance_us(101000); pump(); connect_link();
        if(!midi) midi=(uint16_t)(discover_all()+2u);
        pair_link((int)(id&1u),1,capacity_ltk[id],capacity_rand[id]); subscribe_changed();
        uint8_t q[]={18,0,0,1,0}; ble_put16(q+1,midi+1u); l2send(4,q,5); assert(att[0]==19);
        disconnect_link(); stored(id,1); privacy_ready(id+1u);
        ble_bonds_get_diag(&d); assert(d.count==id && d.capacity==8 && !d.pending && !d.replacements);
    }
    capacity_reboot();
    for(unsigned n=0;n<8;n++) capacity_reconnect(order[n],1);
    for(unsigned n=0;n<8;n++) { assert(!ble_bonds_peer(n,&peer)); assert(peer.addr[0]==order[7-n] && peer.rank==n+1u); }
    unsigned w=writes,e=erases;
    for(unsigned n=0;n<3;n++) capacity_reconnect(5,n+2u);
    assert(writes==w && erases==e);
    /* An attempted third CCCD cannot evict unrelated identities. */
    struct ble_store_value_cccd third={0}; third.peer_addr.val[0]=5; third.chr_val_handle=0x999; third.flags=1;
    assert(ble_store_write_cccd(&third)==BLE_HS_ESTORE_CAP);
    third.peer_addr.val[0]=10; third.chr_val_handle=0;
    assert(ble_store_write_cccd(&third)==BLE_HS_EINVAL); /* cannot trigger eviction retries */
    ble_bonds_get_diag(&d); assert(!d.replacements && !d.pending);
    select_peer(9,0); sim_advance_us(101000); pump(); connect_link();
    pair_link(1,1,capacity_ltk[9],capacity_rand[9]); subscribe_changed();
    uint8_t q[]={18,0,0,1,0}; ble_put16(q+1,midi+1u); l2send(4,q,5); assert(att[0]==19);
    disconnect_link();
    ble_bonds_get_diag(&d); assert(d.count==8 && d.replacements==1 && !d.pending);
    stored(4,0); for(unsigned id=1;id<=9;id++) if(id!=4) stored(id,1);
    /* Real 2028 removed the victim; new 2027/204E plus local entry fill 9. */
    privacy_ready(9); capacity_reboot();
    ble_bonds_get_diag(&d); assert(d.replacements==1);
    for(unsigned n=0;n<8;n++) { assert(!ble_bonds_peer(n,&peer)); assert(peer.addr[0]==(n ? order[8-n] : 9)); }
    /* Missing identity's fresh RPA is unresolved and gets a negative HCI LTK
     * reply, while all eight survivors resolve, encrypt, restore both CCCDs. */
    select_peer(4,9); sim_advance_us(101000); pump(); connect_link(); assert(!ble_link.peer_resolved);
    ble_security_diag_t enc; ble_security_get_diag(&enc); unsigned negatives=enc.negative_replies;
    missing_expected=1; missing_seen=0; memset(request,0,23); request[0]=3;
    memcpy(request+1,capacity_rand[4],10); memset(request+11,0x33,12); control_rx(request,23);
    ble_security_get_diag(&enc); assert(missing_seen && enc.negative_replies==negatives+1u && !started);
    missing_expected=0; disconnect_link();
    for(unsigned id=1;id<=9;id++) if(id!=4) capacity_reconnect(id,10);
    ble_host_get_diag(&h); assert(!h.error && !h.pair_failures && !h.assertions && !sc_indications);
    assert(BLE_OBJECT_SIZE<=3840 && BLE_RL_CAPACITY>=BLE_BOND_CAPACITY+2u);
    puts("F-29: 8 real mixed SC/legacy SMP bonds + both CCCDs, reload + HCI resync, known LRU order, 3 repeats ZERO programs/erases, real ninth-pair overflow/2028/2027/204E, exact victim negative LTK, second reload, persistent ranks/replacement count, all 8 surviving RPA/CCCD/encrypted reconnects: PASS");
}
