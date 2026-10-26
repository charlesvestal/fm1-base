/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* Actual pinned host SMP (legacy and SC), actual HCI/LL/CCM, deterministic
 * test-only central and NOR model. No hardware or network I/O. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "tinycrypt/ecc.h"
#include "../third_party/nimble/nimble/host/src/ble_sm_priv.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "../third_party/nimble/nimble/host/src/ble_hs_priv.h"
#pragma GCC diagnostic pop
#include "ble_s3_sim.h"
#include "../open_ble/ble_host.h"
#include "../open_ble/ble_crypto.h"
#include "../open_ble/ble_security.h"
#include "../open_ble/ble_seed.h"
#include "../open_ble/ble_bonds.h"
#if FELUCCA_BLE_S31
#include "../open_ble/ble_privacy.h"
#endif
static const uint8_t pub[6]={0x58,0x65,0x67,0x8b,0x40,0x62};
static uint8_t central[6]={1,0,0,0,0,0};
static int missing_expected,missing_seen;
static uint8_t smp[32][70],smp_len[32],l2[524],att[524];
static uint8_t signaling[16];
static unsigned signaling_len;
static unsigned smp_n,l2_n,l2_want,att_n,fragments65,sc_indications;
static uint8_t sc_ack_due;
static int auto_confirm=1;
static int peer_terminate_ok;                  /* the FM-1 may end the link (ble_open_flash_jobs_test.c) */
static unsigned peer_terminates;
static uint8_t peer_terminate_reason;
static uint8_t session_ltk[16],sk[16],iv[8],request[23];
static uint64_t tx_counter,rx_counter;
static int tx_encrypted,started,start_req_seen;
static int reconnect_rpa;
static uint8_t current_rpa[6]={0xaa,0xfb,0x0d,0x94,0x81,0x70};
static uint8_t peer_irk[16];
static const uint8_t sample_irk[16]={0x9b,0x7d,0x39,0x0a,0xa6,0x10,0x10,0x34,5,0xad,0xc8,0x57,0xa3,0x34,2,0xec};
static void control_rx(const uint8_t *p,unsigned n);
static void central_rx(const uint8_t *p,unsigned n,uint8_t llid)
{
    uint8_t air[31]; assert(n<=27); memcpy(air,p,n);
    if(tx_encrypted && n) { assert(ble_ccm(sk,iv,tx_counter++,1,llid,air,(uint8_t)n,0)); n+=4; }
    assert(ble_ll_rx(1,(uint16_t)((n<<8)|llid),air,sim_ticks,0));
}
static void take_slot(unsigned slot)
{
    unsigned id=(unsigned)sim_slot[slot],n=sim_air_len[slot],llid=ble_ll_tx_llid(id),i; uint8_t p[31];
    memcpy(p,sim_air[slot],n);
    if(n==ble_ll_tx_len(id)+4u) { assert(ble_ccm(sk,iv,rx_counter++,0,(uint8_t)llid,p,(uint8_t)(n-4u),1)); n-=4; }
    if(llid==3) {
        if(p[0]==4 && n==13) {
            uint8_t skd[16],keyle[16]; memcpy(skd,request+11,8); memcpy(skd+8,p+1,8);
            memcpy(iv,request+19,4); memcpy(iv+4,p+9,4); assert(ble_aes_le(session_ltk,skd,keyle));
            for(i=0;i<16;i++) sk[i]=keyle[15u-i];
        } else if(p[0]==5) start_req_seen=1;
        else if(p[0]==6) started=1;
        else if(p[0]==13 && p[1]==6 && missing_expected) missing_seen=1;
        else if(p[0]==2 && n==2 && peer_terminate_ok) { peer_terminates++; peer_terminate_reason=p[1]; }
        else if(p[0]==13 || p[0]==2) { fprintf(stderr,"unexpected LL reject/terminate %u reason %u\n",p[0],p[1]); assert(0); }
    } else {
        if(llid==2) { l2_n=0; l2_want=ble_get16(p)+4u; assert(l2_want<=sizeof l2); }
        assert(l2_n+n<=sizeof l2); memcpy(l2+l2_n,p,n); l2_n+=n;
        if(l2_n==l2_want) {
            if(ble_get16(l2+2)==6) {
                assert(smp_n<32 && l2_n-4u<=70); smp_len[smp_n]=(uint8_t)(l2_n-4u);
                memcpy(smp[smp_n++],l2+4,l2_n-4u); if(l2_n==69) fragments65++;
            } else if(ble_get16(l2+2)==5) {
                signaling_len=l2_n-4u; assert(signaling_len<=sizeof signaling);
                memcpy(signaling,l2+4,signaling_len);
            } else if(ble_get16(l2+2)==4) { att_n=l2_n-4u; memcpy(att,l2+4,att_n);
                if(att[0]==0x1d) {
                    assert(att_n==7 && ble_get16(att+3)==1 && ble_get16(att+5)==0xffff);
                    sc_indications++; sc_ack_due=1;
                } }
            l2_n=l2_want=0;
        }
    }
    sim_ack(slot);
}
static void pump(void)
{
    unsigned i,j;
    for(i=0;i<
#if FM1B_BLE_RELIABILITY
        512u /* bounded incremental CCM classification and TX precompute */
#else
        32u
#endif
        ;i++) {
        int any=0;
        sim_advance_us(0); ble_host_worker(); ble_port_refill();
        for(j=0;j<2;j++) if(sim_slot[j]>=0) { take_slot(j); any=1; }
        if(!any && !ble_ll_pending()) break;
    }
}
static void l2send(uint16_t cid,const uint8_t *p,unsigned n)
{
    uint8_t data[524]; unsigned off=0;
    ble_put16(data,(uint16_t)n); ble_put16(data+2,cid); memcpy(data+4,p,n); n+=4;
    while(off<n) { unsigned take=n-off>27 ? 27 : n-off; central_rx(data+off,take,off ? 1 : 2); off+=take; if(n>256u) pump(); }
    pump();
}
static unsigned smp_get(uint8_t op,uint8_t *out)
{
    unsigned i; pump();
    for(i=0;i<smp_n;i++) if(smp_len[i] && smp[i][0]==op) { unsigned n=smp_len[i]; memcpy(out,smp[i],n); smp_len[i]=0; return n; }
    { ble_host_diag_t d; ble_host_get_diag(&d); fprintf(stderr,"host req %u err %u conn %u ms %u state %u pairfail %u acldrop %u procs %d; ",d.security_requests,d.error,d.connection,ble_port_ms(),ble_link.state,d.pair_failures,d.acl_drop,ble_sm_num_procs()); }
    { ble_seed_diag_t seed; ble_host_diag_t d; ble_seed_get_diag(&seed); ble_host_get_diag(&d); fprintf(stderr,"seed %u Rand %u status %u asserts %u; ",seed.state,seed.rand_calls,d.pair_status,d.assertions); }
    fprintf(stderr,"missing SMP %02x; received:",op); for(i=0;i<smp_n;i++) fprintf(stderr," %02x",smp[i][0]); fputc('\n',stderr); assert(0); return 0;
}
static void smp_send(uint8_t op,const uint8_t *p,unsigned n)
{
    uint8_t data[70]; data[0]=op; memcpy(data+1,p,n); l2send(6,data,n+1);
}
static void control_rx(const uint8_t *p,unsigned n) { central_rx(p,n,3); pump(); }
#include "ble_nimble110_test.inc"
static void connect_link(void)
{
    ble_conn_t c=sim_connection();
    if(reconnect_rpa) { c.peer_type=1; memcpy(c.peer,current_rpa,6); }
    tx_encrypted=started=start_req_seen=0; tx_counter=rx_counter=0; smp_n=l2_n=l2_want=0; sim_slot[0]=sim_slot[1]=-1;
    assert(ble_link.state==BLE_LINK_IDLE && ble_ll_create(&c,sim_ticks)); assert(ble_ll_rx(1,1,0,sim_ticks,0)); pump();
    control_rx((uint8_t[9]){8,1,0,0,0,0,0,0,0},9);
    control_rx((uint8_t[6]){12,6,1,0,2,0},6);
    observe_connection();
}
static void encryption_start(const uint8_t key[16],const uint8_t rand_ediv[10])
{
    unsigned i; memcpy(session_ltk,key,16); memset(request,0,sizeof request); request[0]=3;
    memcpy(request+1,rand_ediv,10); for(i=0;i<12;i++) request[11+i]=(uint8_t)(i+33);
    control_rx(request,23); pump();
    if(!start_req_seen) {
        ble_host_diag_t d; ble_security_diag_t enc; ble_host_get_diag(&d); ble_security_get_diag(&enc);
        fprintf(stderr,"encryption failed: RPA %02x IRK %02x pair status %u failures %u error %u phase %u negative %u SMP",current_rpa[3],peer_irk[0],d.pair_status,d.pair_failures,d.error,enc.phase,enc.negative_replies);
        fprintf(stderr," state %u reason %u peerstop %u conn %u creates %u ",ble_link.state,ble_link.reason,ble_link.peer_stop,d.connection,ble_link.diag.created);
        for(i=0;i<smp_n;i++) fprintf(stderr," %02x/%02x",smp[i][0],smp[i][1]);
        fputc('\n',stderr);
    }
    assert(start_req_seen);
    tx_encrypted=1; tx_counter=rx_counter=0; control_rx((uint8_t[]){6},1);
    assert(started);
    if(auto_confirm && sc_ack_due) { sc_ack_due=0; l2send(4,(uint8_t[]){0x1e},1); }
}
static void disconnect_link(void)
{
    control_rx((uint8_t[]){2,0x13},2);
    sim_advance_us(2500); /* First real event may not have closed yet (A8). */
    ble_ll_event(0,sim_ticks); pump(); assert(ble_link.state==BLE_LINK_IDLE);
}
#if FELUCCA_BLE_S31
static void privacy_ready(unsigned entries)
{
    ble_privacy_diag_t privacy;
#if FM1B_BLE_DIAG
    static uint32_t mask_verified_sync;
    ble_host_diag_t host;
    unsigned i; int found=0;
#endif
    ble_privacy_get_diag(&privacy); assert(privacy.enabled && privacy.count==entries);
#if FM1B_BLE_DIAG
    ble_host_get_diag(&host);
    /* ECDH blinding adds LE Rand traffic. Across eight real pairings the
     * finite trace can evict the startup mask; verify it once per sync while
     * fresh, and continue checking live privacy state on every invocation. */
    if(mask_verified_sync==host.syncs) return;
    ble_trace_freeze(1);
    for(i=ble_trace_size();i;i--) {
        const ble_trace_t *t=ble_trace_record(i-1);
        if(t->dir_type==129 && ble_get16(t->data)==0x2001) {
            assert(t->len==11 && !memcmp(t->data+3,(uint8_t[]){0x1f,2,0,0,0,0,0,0},8)); found=1; break;
        }
    }
    ble_trace_freeze(0); assert(found);
    mask_verified_sync=host.syncs;
#else
    assert(!ble_trace_size() && !ble_trace_record(0));
#endif
}
#endif
/* A real ATT client must continue across 16/128/16-bit UUID response pages.
 * This is also the full-range rediscovery a BlueZ stale cache must perform. */
static uint16_t discover_all(void)
{
    uint16_t next=1,midi=0,last=0; unsigned services=0,mask=0;
    for(unsigned page=0;page<8;page++) {
        uint8_t q[]={0x10,0,0,255,255,0,0x28}; ble_put16(q+1,next);
        l2send(4,q,sizeof q);
        if(att[0]==1) { assert(att[4]==BLE_ATT_ERR_ATTR_NOT_FOUND); break; }
        assert(att[0]==0x11 && (att[1]==6 || att[1]==20));
        unsigned len=att[1]; assert((att_n-2u)%len==0);
        for(unsigned i=2;i<att_n;i+=len) {
            uint16_t start=ble_get16(att+i),end=ble_get16(att+i+2);
            assert(start>=next && start>last && end>=start); last=start; services++;
            if(len==6) {
                uint16_t uuid=ble_get16(att+i+4);
                if(uuid==0x1800) { assert(start==1 && end==5); mask|=1; }
                else if(uuid==0x1801) { assert(start==6 && end==13); mask|=2; }
                else { assert(uuid==0x180a && start==18); mask|=8; }
            } else {
                static const uint8_t uuid[]={0x00,0xc7,0xc4,0x4e,0xe3,0x6c,0x51,0xa7,0x33,0x4b,0xe8,0xed,0x5a,0x0e,0xb8,0x03};
                assert(!memcmp(att+i+4,uuid,16)); midi=start; mask|=4;
                assert(start==14 && end==17); /* R2 appends DIS after MIDI. */
            }
            if(end==0xffff) goto done;
            next=(uint16_t)(end+1u);
        }
    }
done:
    assert(services==4 && mask==15 && midi);
    return midi;
}
static uint16_t changed_handle(void)
{
    uint16_t h;
    assert(!ble_gatts_find_chr(BLE_UUID16_DECLARE(0x1801),BLE_UUID16_DECLARE(0x2a05),0,&h));
    assert(h==8); return h;
}
static void subscribe_changed(void)
{
    uint8_t q[]={18,0,0,2,0}; ble_put16(q+1,changed_handle()+1u); l2send(4,q,sizeof q);
}
static void check_midi_read(uint16_t value,int encrypted)
{
    uint8_t q[]={10,0,0}; ble_put16(q+1,value); l2send(4,q,sizeof q);
    if(encrypted) assert(att_n==1 && att[0]==11);
    else assert(att_n==5 && att[0]==1 && att[1]==10 && att[4]==BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
}
static void pair_link(int sc,int rpa,uint8_t saved_ltk[16],uint8_t rand_ediv[10])
{
    uint8_t preq[7]={1,3,0,1,16,1,1},pres[70],nc[16],np[16],confirm[70],packet[70],key[16],zero[16]={0};
    uint8_t pubc[64],privc[32]={2},pubp[64],dhkey[32],mackey[16],expected[16];
    unsigned i,fragments_before=fragments65; ble_host_diag_t d;
    unsigned completions_before=persisted_completions;
    expect_pair_complete=1; expected_sc=(unsigned)sc;
    memset(rand_ediv,0,10);
    if(rpa) preq[5]=preq[6]=3;
    for(i=0;i<16;i++) nc[i]=(uint8_t)(i+80);
    preq[3]=(uint8_t)(sc ? 9 : 1); l2send(6,preq,7); assert(smp_get(2,pres)==7 && pres[4]==16);
    if(rpa && !reconnect_rpa) {
        sim_advance_us(101000); assert(ble_ll_rx(1,1,0,sim_ticks,0)); pump();
        ble_host_get_diag(&d); assert(!d.security_requests && !d.error && !d.pair_failures);
    }
    if(sc) {
        uint8_t privbe[32]={0},pubbe[64]; privbe[31]=2;
        /* Test-only central private scalar 2. Production host still generates its
         * key through LE Rand; no host debug keys or RNG hook are enabled. */
        assert(uECC_compute_public_key(privbe,pubbe,&curve_secp256r1));
        for(i=0;i<32;i++) { pubc[i]=pubbe[31-i]; pubc[32+i]=pubbe[63-i]; }
        smp_send(12,pubc,64); assert(smp_get(12,packet)==65); memcpy(pubp,packet+1,64);
        assert(smp_get(3,confirm)==17); smp_send(4,nc,16); assert(smp_get(4,packet)==17); memcpy(np,packet+1,16);
        assert(!ble_sm_alg_f4(pubp,pubc,np,0,expected) && !memcmp(expected,confirm+1,16));
        assert(!ble_sm_alg_gen_dhkey(pubp,pubp+32,privc,dhkey));
        assert(!ble_sm_alg_f5(dhkey,nc,np,ble_link.conn.peer_type,ble_link.conn.peer,0,pub,mackey,key));
        assert(!ble_sm_alg_f6(mackey,nc,np,zero,preq+1,ble_link.conn.peer_type,ble_link.conn.peer,0,pub,expected));
        smp_send(13,expected,16); assert(smp_get(13,packet)==17);
        assert(!ble_sm_alg_f6(mackey,np,nc,zero,pres+1,0,pub,ble_link.conn.peer_type,ble_link.conn.peer,expected) && !memcmp(expected,packet+1,16));
        assert(fragments65==fragments_before+1u);
    } else {
        assert(!ble_sm_alg_c1(zero,nc,preq,pres,ble_link.conn.peer_type,0,ble_link.conn.peer,pub,expected)); smp_send(3,expected,16);
        assert(smp_get(3,confirm)==17); smp_send(4,nc,16); assert(smp_get(4,packet)==17); memcpy(np,packet+1,16);
        assert(!ble_sm_alg_c1(zero,np,preq,pres,ble_link.conn.peer_type,0,ble_link.conn.peer,pub,expected) && !memcmp(expected,confirm+1,16));
        assert(!ble_sm_alg_s1(zero,np,nc,key));
    }
    encryption_start(key,rand_ediv);
    if(!sc) {
        assert(smp_get(6,packet)==17); memcpy(saved_ltk,packet+1,16);
        assert(smp_get(7,packet)==11); memcpy(rand_ediv,packet+3,8); memcpy(rand_ediv+8,packet+1,2);
        memset(packet,0x5a,16); smp_send(6,packet,16); memset(packet,0x6b,10); smp_send(7,packet,10);
    } else memcpy(saved_ltk,key,16);
    if(rpa) {
        assert(smp_get(8,packet)==17); assert(smp_get(9,packet)==8);
        smp_send(8,peer_irk,16); packet[0]=0; memcpy(packet+1,central,6); smp_send(9,packet,7);
    }
    pump(); ble_host_get_diag(&d);
    assert(d.encrypted && d.key_size==16 && d.bonded && d.pair_method==(sc ? 2 : 1) && !d.pair_failures );
    assert(!expect_pair_complete && persisted_completions==completions_before+1u);
}
#if FM1B_BLE_OPEN
#include "ble_bond_capacity_test.inc"
#endif
int main(int argc,char **argv)
{
#if FM1B_BLE_OPEN
    if(argc>1 && !strcmp(argv[1],"f29")) { capacity_test(); return 0; }
#endif
    uint8_t packet[70],saved_ltk[16],rand_ediv[10]={0};
    (void)packet;
    int repeat=argc>1 && (!strcmp(argv[1],"repeat-legacy") || !strcmp(argv[1],"repeat-sc"));
    (void)repeat;
    int sc=argc>1 && (!strcmp(argv[1],"sc") || !strcmp(argv[1],"repeat-sc"));
    int rpa=repeat || (argc>1 && !strcmp(argv[1],"rpa"));
    ble_host_diag_t d; ble_seed_diag_t seed; ble_bond_diag_t bond;
    memcpy(peer_irk,sample_irk,16);
    sim_reset(); sim_storage_reset(); sim_seed(); sim_executor_enabled=1; ble_init(pub,0); ble_host_start(); pump(); sim_advance_us(10000); pump();
    ble_host_get_diag(&d); assert(d.synced && !d.error);
#if FELUCCA_BLE_S31
    privacy_ready(1);
#endif
    connect_link();
    if(!rpa) {
        sim_advance_us(101000); assert(ble_ll_rx(1,1,0,sim_ticks,0)); pump();
        for(unsigned j=0;j<smp_n;j++) assert(smp[j][0]!=11); /* no unsolicited Security Request */
    }
    /* Apple-style first setup: public discovery + Service Changed subscription,
     * protected MIDI read -> central SMP -> empty retried read -> CCCD write. */
    uint16_t midi_start=discover_all(),midi_value=(uint16_t)(midi_start+2u);
    check_midi_read(midi_value,0);
    protected_cccd_before_pairing(midi_value);
    att_110_regressions(midi_value);
    subscribe_changed(); assert(!sc_indications);
    pair_link(sc,rpa,saved_ltk,rand_ediv);
    puts("NimBLE 1.10 behavior: protected CCCD rejects unencrypted write and remains readable, long Read By Type, exact Find By Type Value, MTU 517, keys visible in pairing-complete callback: PASS");
    check_midi_read(midi_value,1); assert(!sc_indications);
    if(rpa) {
        union ble_store_value other; memset(&other,0,sizeof other);
        other.sec.peer_addr.val[0]=2; other.sec.key_size=16; other.sec.ltk_present=1; other.sec.sc=!sc;
        assert(!ble_bonds_write(1,&other)); ble_host_get_diag(&d);
        assert(d.identity_known && d.pair_method==(sc ? 2 : 1) && d.sc==sc); /* latest write belongs to the other peer */
    }
    /* Open ATT read still works encrypted; timestamped write flows to MIDI. */
    ble_host_get_diag(&d); assert(d.mtu==517); /* exchanged by the ATT regression */
    /* Each of the host and test central now supplies a fresh 32-byte ECDH
     * blinding Z through the per-call RNG: eight additional LE Rand calls. */
    ble_seed_get_diag(&seed); assert(seed.rand_calls==(sc ? 18u : 6u));
    {
        static const uint8_t uuid[16]={0x00,0xc7,0xc4,0x4e,0xe3,0x6c,0x51,0xa7,0x33,0x4b,0xe8,0xed,0x5a,0x0e,0xb8,0x03};
        uint8_t q[32]={6,1,0,255,255,0,0x28}; uint16_t start,end,value,cccd; uint32_t pkt,ts;
        memcpy(q+7,uuid,16); l2send(4,q,23); assert(att[0]==7); start=ble_get16(att+1); end=ble_get16(att+3);
        q[0]=8; ble_put16(q+1,start); ble_put16(q+3,end); ble_put16(q+5,0x2803); l2send(4,q,7);
        assert(att[0]==9); value=ble_get16(att+5);
        q[0]=4; ble_put16(q+1,(uint16_t)(value+1u)); ble_put16(q+3,end); l2send(4,q,5); assert(att[0]==5); cccd=ble_get16(att+2);
        q[0]=18; ble_put16(q+1,cccd); q[3]=1; q[4]=0; l2send(4,q,5); assert(att[0]==19);
        q[0]=0x52; ble_put16(q+1,value); memcpy(q+3,(uint8_t[]){0x80,0x80,0x90,60,100},5); l2send(4,q,8);
        assert(ble_host_midi_take(&pkt,&ts) && pkt==0x643c9009);
        ble_host_midi_out(pkt,8191); pump(); assert(att[0]==0x1b && ble_get16(att+1)==value && att[3]==0xbf && att[4]==0xff);
        /* Largest negotiated MIDI value, worst-case USB expansion: the whole
         * packet must fit without overflow before the app gets a turn. */
        uint8_t large[515]={0x52}; ble_put16(large+1,value);
        large[3]=large[4]=0x80; large[5]=0xc0;
        l2send(4,large,sizeof large);
        for(unsigned j=0;j<509;j++) assert(ble_host_midi_take(&pkt,&ts) && pkt==0xc00cu);
        assert(!ble_host_midi_take(&pkt,&ts));
        ble_host_get_diag(&d); assert(!d.midi_drop && !d.midi_bad);

    }
    subscribe_changed();
    assert(!sc_indications && !sc_ack_due); /* New bonds have no stale cache. */
    ble_host_get_diag(&d); assert(!d.gatt_invalidations && d.mtu==517);
#if FM1B_BLE_DIAG
    {
        ble_host_event_t events[BLE_HOST_EVENT_COUNT];
        unsigned n=ble_host_events(events),reject=0,start=0,result=0,read=0,cccd=0;
        for(unsigned i=0;i<n;i++) {
            assert(!i || events[i].ms>=events[i-1].ms);
            if(events[i].kind==BLE_HE_ATT_REJECT) reject=i+1;
            if(events[i].kind==BLE_HE_SMP_START) start=i+1;
            if(events[i].kind==BLE_HE_SMP_RESULT) { result=i+1; assert(events[i].b==(sc ? 2u : 1u) && !events[i].a); }
            if(events[i].kind==BLE_HE_MIDI_READ) read=i+1;
            if(events[i].kind==BLE_HE_CCCD_WRITE && events[i].a==midi_value+1u) cccd=i+1;
            assert(events[i].kind!=BLE_HE_SC_SENT);
        }
        assert(reject && start>reject && result>start && read>result && cccd>read);
        /* CCCD reads must not be labelled writes; a rejected write retains
         * its handle/value/error and does not alter the current subscription. */
        unsigned before=ble_host_events(events);
        uint8_t q[]={10,0,0}; ble_put16(q+1,midi_value+1u); l2send(4,q,sizeof q);
        assert(att[0]==11 && ble_get16(att+1)==1);
        assert(ble_host_events(events)==before);
        uint8_t invalid[]={18,0,0,2,0}; ble_put16(invalid+1,midi_value+1u);
        l2send(4,invalid,sizeof invalid); assert(att[0]==1 && att[4]==BLE_ATT_ERR_REQ_NOT_SUPPORTED);
        n=ble_host_events(events);
        assert(events[n-1].kind==BLE_HE_CCCD_WRITE && events[n-1].a==midi_value+1u &&
               events[n-1].b==2 && events[n-1].c==BLE_ATT_ERR_REQ_NOT_SUPPORTED);
    }
#endif
    disconnect_link();
    ble_bonds_get_diag(&bond); assert(!bond.pending && bond.writes && !bond.failures);
    /* Reconstruct the RAM cache from the NOR journal (simulated reboot of
     * store, not a second NimBLE init) and encrypt with its persistent LTK. */
    ble_bonds_init();
#if FELUCCA_BLE_S31
    if(rpa) {
        /* Real host Reset/resync restores the persisted IRK before advertising. */
        ble_host_resume(); pump(); sim_advance_us(101000); pump();
        privacy_ready(2);
        reconnect_rpa=1;
    }
#endif
    sim_advance_us(101000); pump(); connect_link(); encryption_start(saved_ltk,rand_ediv);
    ble_host_get_diag(&d); assert(d.encrypted && d.bonded && d.key_size==16 && !d.pair_failures);
    assert(!sc_indications && !sc_ack_due); /* Same database: quiet after restart. */
    if(rpa) {
        assert(d.identity_known && d.sc==sc && d.pair_method==(sc ? 2 : 1) && !memcmp(d.peer_identity,central,6));
#if FELUCCA_BLE_S31
        { ble_privacy_diag_t privacy; ble_privacy_get_diag(&privacy); assert(privacy.matches==1); }
#endif
        { ble_security_diag_t enc; ble_security_get_diag(&enc); assert(!enc.negative_replies); }
        { unsigned requests=d.security_requests; sim_advance_us(101000); pump(); ble_host_get_diag(&d);
          assert(d.security_requests==requests); }
    }
    /* Pre-R3 bonded cache: generation zero, R2's persisted global changed bit,
     * old name and only GAP/GATT/DIS in the central cache. No re-pairing. */
    disconnect_link();
    assert(!ble_bonds_db_confirm(0,central,0));
    {
        union ble_store_value v; memset(&v,0,sizeof v);
        v.cccd.peer_addr.type=0; memcpy(v.cccd.peer_addr.val,central,6);
        v.cccd.chr_val_handle=changed_handle(); v.cccd.flags=2; v.cccd.value_changed=1;
        assert(!ble_bonds_write(BLE_STORE_OBJ_TYPE_CCCD,&v));
    }
    ble_bonds_flush(); ble_bonds_init();
    sim_advance_us(101000); pump(); connect_link();
    /* Before encryption, all discovery pages, including MIDI, are public. */
    assert(discover_all()==midi_start);
    auto_confirm=0; encryption_start(saved_ltk,rand_ediv);
    assert(sc_indications==1 && sc_ack_due);
    assert(discover_all()==midi_start); /* Rediscovery while indication is outstanding. */
    pump(); assert(sc_indications==1);
    uint32_t version; assert(!ble_bonds_db_version(0,central,&version) && !version);
    /* Losing an unconfirmed indication must leave this peer stale. */
    sc_ack_due=0; disconnect_link(); ble_bonds_init();
    sim_advance_us(101000); pump(); connect_link(); encryption_start(saved_ltk,rand_ediv);
    assert(sc_indications==2 && sc_ack_due);
    sc_ack_due=0; l2send(4,(uint8_t[]){0x1e},1); auto_confirm=1;
    assert(!ble_bonds_db_version(0,central,&version) && version==BLE_GATT_DB_VERSION);
    assert(discover_all()==midi_start);
    check_midi_read(midi_value,1);
    /* The refreshed BlueZ-style cache drives the real MIDI characteristic in
     * both directions, and its public GAP read refreshes the old name. */
    {
        uint8_t q[]={8,1,0,255,255,0,0x2a}; l2send(4,q,sizeof q);
        assert(att[0]==9 && !memcmp(att+4,"FM-1 Open BLE",13));
        uint8_t sub[]={18,0,0,1,0}; ble_put16(sub+1,midi_value+1u); l2send(4,sub,sizeof sub);
        assert(att[0]==19);
        uint8_t write[]={0x52,0,0,0x80,0x80,0x90,60,100}; ble_put16(write+1,midi_value);
        l2send(4,write,sizeof write);
        uint32_t pkt,ts; assert(ble_host_midi_take(&pkt,&ts) && pkt==0x643c9009);
        ble_host_midi_out(pkt,0); pump(); assert(att[0]==0x1b && ble_get16(att+1)==midi_value);
    }
    for(unsigned i=0;i<32;i++) pump();
    assert(sc_indications==2); /* Exactly one sent in each stale connection. */
    disconnect_link(); ble_bonds_init();
    sim_advance_us(101000); pump(); connect_link(); encryption_start(saved_ltk,rand_ediv);
    assert(sc_indications==2 && !sc_ack_due); /* Confirmation survived store reload. */
    ble_host_get_diag(&d); assert(d.gatt_invalidations==2 && d.gatt_confirmations==1);
    /* A peer that disabled 0x2a05 indications stays stale until it opts in.
     * Discovery remains public even without its cache subscription. */
    disconnect_link(); assert(!ble_bonds_db_confirm(0,central,0));
    {
        union ble_store_key k; memset(&k,0,sizeof k);
        k.cccd.peer_addr.type=0; memcpy(k.cccd.peer_addr.val,central,6);
        k.cccd.chr_val_handle=changed_handle(); assert(!ble_bonds_delete(3,&k));
    }
    ble_bonds_flush(); ble_bonds_init();
    sim_advance_us(101000); pump(); connect_link(); encryption_start(saved_ltk,rand_ediv);
    assert(sc_indications==2 && !sc_ack_due);
    assert(!ble_bonds_db_version(0,central,&version) && !version);
    assert(discover_all()==midi_start);
    subscribe_changed(); assert(sc_indications==3 && sc_ack_due);
    sc_ack_due=0; l2send(4,(uint8_t[]){0x1e},1); pump(); assert(sc_indications==3);
    if(rpa) {
        const uint8_t other[6]={2,0,0,0,0,0};
        assert(!ble_bonds_db_version(0,other,&version) && version==BLE_GATT_DB_VERSION);
        union ble_store_key k; union ble_store_value v; memset(&k,0,sizeof k);
        k.cccd.peer_addr.type=0; memcpy(k.cccd.peer_addr.val,other,6);
        k.cccd.chr_val_handle=changed_handle();
        assert(ble_bonds_read(3,&k,&v)==BLE_HS_ENOENT); /* No global fanout to another bond. */
    }
    puts("R3 GATT clients: public mixed-UUID discovery, protected read/SMP/empty read/CCCD, new bond quiet, stale R2 pending-bit migration, unconfirmed retry, confirmed bond quiet across reconnect: PASS");
#if FM1B_BLE_DIAG
    {
        ble_host_event_t events[BLE_HOST_EVENT_COUNT]; unsigned n=ble_host_events(events);
        assert(n==BLE_HOST_EVENT_COUNT);
        for(unsigned i=1;i<n;i++) assert(events[i].session>=events[i-1].session);
    }
#endif
#if FELUCCA_BLE_S31
    if(repeat) {
        /* Initial bond exists on both sides and has just reconnected. The
         * central now discards only its saved LTK, retaining the same IRK. */
        uint8_t old_ltk[16]; unsigned round,j;
#if FM1B_BLE_DIAG
        uint32_t trace_before;
#endif
        for(round=0;round<3;round++) {
            disconnect_link();
            sim_advance_us(101000); pump();
            if(round) peer_irk[0]^=0x55; /* New identity key, same identity addr. */
            /* Round 1 retains its old RPA while distributing a new IRK.
             * Round 2 arrives with a new RPA under the changed IRK, so its
             * identity becomes known only during SMP key distribution. */
            if(round==2) { current_rpa[3]=0x91; assert(ble_ah(peer_irk,current_rpa+3,current_rpa)); }
            connect_link();
            { struct ble_gap_conn_desc desc; assert(!ble_gap_conn_find(BLE_HANDLE,&desc));
              assert(desc.peer_ota_addr.type==1 && !memcmp(desc.peer_ota_addr.val,current_rpa,6));
              if(round==2) assert(desc.peer_id_addr.type==1 && !memcmp(desc.peer_id_addr.val,current_rpa,6));
              else assert(!desc.peer_id_addr.type && !memcmp(desc.peer_id_addr.val,central,6)); }
#if FM1B_BLE_DIAG
            { ble_diag_t ctl; ble_get_diag(&ctl); trace_before=ctl.trace_count; }
#endif
            memcpy(old_ltk,saved_ltk,16);
            pair_link(sc,1,saved_ltk,rand_ediv);
            assert(memcmp(old_ltk,saved_ltk,16));
            /* Trace the real repeat callback: one 2028 BEFORE one 2027,
             * successful replies, and no Disconnect (0406). */
#if FM1B_BLE_DIAG
            { unsigned removed=0,added=0,k; ble_diag_t ctl; ble_get_diag(&ctl);
              unsigned delta=ctl.trace_count-trace_before; assert(delta<=ble_trace_size()); ble_trace_freeze(1);
              for(k=ble_trace_size()-delta;k<ble_trace_size();k++) {
                  const ble_trace_t *t=ble_trace_record(k);
                  if(t->dir_type==129 && t->len>=3) {
                      uint16_t op=ble_get16(t->data);
                      if(op==0x2028) { assert(!added); removed++; }
                      if(op==0x2027) { assert(removed==1); added++; }
                      assert(op!=0x0406);
                  }
                  if(t->dir_type==4 && t->data[0]==14 &&
                     (ble_get16(t->data+3)==0x2028 || ble_get16(t->data+3)==0x2027)) assert(!t->data[5]);
              }
              assert(removed==1 && added==1); ble_trace_freeze(0); }
#endif
            for(j=0;j<3;j++) {
                disconnect_link();
                sim_advance_us(101000); pump();
                if(j==1) {
                    /* Normal host HCI-error Reset/resync, in addition to the
                     * console resume path exercised before the first RPA. */
                    ble_hs_sched_reset(BLE_HS_ECONTROLLER); pump(); sim_advance_us(101000); pump(); privacy_ready(2);
                }
                current_rpa[3]=(uint8_t)(0x80u+round*8u+j); current_rpa[4]=0x81; current_rpa[5]=0x70;
                assert(ble_ah(peer_irk,current_rpa+3,current_rpa));
                connect_link(); encryption_start(saved_ltk,rand_ediv);
                ble_host_get_diag(&d); assert(d.encrypted && d.bonded && !d.pair_failures && !d.error);
                { ble_privacy_diag_t privacy; ble_privacy_get_diag(&privacy); assert(privacy.count==2 && privacy.enabled); }
            }
        }
        /* Exercise the installed replacement callback via a real capacity
         * overflow, then explicit application unpair (including absent IRK).
         * The encrypted connection must remain usable throughout. */
#if FM1B_BLE_OPEN
        { struct ble_store_value_sec extra={0}; union ble_store_key k={0}; union ble_store_value v;
          extra.key_size=16; extra.ltk_present=1; extra.ltk[0]=0xa5;
          for(unsigned id=3;id<=9;id++) { extra.peer_addr.val[0]=(uint8_t)id; assert(!ble_store_write_our_sec(&extra)); }
          k.sec.peer_addr.val[0]=2; assert(ble_bonds_read(1,&k,&v)==BLE_HS_ENOENT);
          k.sec.peer_addr.val[0]=1; assert(!ble_bonds_read(1,&k,&v)); /* connected oldest is protected */
          assert(!ble_host_delete_bond(0,central) && !ble_host_delete_bond(0,central));
          privacy_ready(1); l2send(4,(uint8_t[]){2,200,0},3); assert(att_n==3 && att[0]==3);
          ble_host_get_diag(&d); assert(d.connection==BLE_HANDLE && d.encrypted && !d.pair_failures && !d.error); }
#else
        { struct ble_store_value_sec third={0}; ble_addr_t peers[2]; int count;
          union ble_store_key k={0}; union ble_store_value v;
          assert(!ble_store_util_bonded_peers(peers,&count,2) && count==2);
          third.peer_addr.val[0]=3; third.key_size=16; third.ltk_present=1; third.ltk[0]=0xa5;
          assert(!ble_store_write_our_sec(&third));
          k.sec.peer_addr=peers[0]; assert(ble_bonds_read(1,&k,&v)==BLE_HS_ENOENT);
          assert(!ble_host_delete_bond(0,central) && !ble_host_delete_bond(0,central));
          privacy_ready(1); l2send(4,(uint8_t[]){2,200,0},3); assert(att_n==3 && att[0]==3);
          ble_host_get_diag(&d); assert(d.connection==BLE_HANDLE && d.encrypted && !d.pair_failures && !d.error); }
#endif
        puts("F-28: real NimBLE repeat pairing, same IRK, changed IRK with old RPA, changed IRK with new unresolved RPA, OTA c1/f5/f6, 2028 before 2027 without disconnect, three fresh-RPA encrypted reconnects each: PASS");
    }
#endif
    /* A corrupted tag ends the physical link before Disconnection Complete. */
    { uint8_t bad[6]={2,0x13}; assert(ble_ccm(sk,iv,tx_counter,1,3,bad,2,0)); bad[5]^=1;
      assert(ble_ll_rx(1,0x603,bad,sim_ticks,0)); pump(); assert(ble_link.reason==0x3d && ble_link.state==BLE_LINK_IDLE); }
    ble_host_get_diag(&d); assert(d.resets==(repeat ? 4u : (unsigned)rpa) && !d.assertions && !d.heap_fail && !d.event_drop && !d.acl_drop);
    printf("NimBLE %s Just Works: real SMP, HCI LTK, CCM link, key size 16, persistent bond reconnect, MTU 517/max MIDI value, Service Changed, encrypted MIDI both directions, MIC failure: PASS; LE Rand %u, heap peak %u\n",rpa ? (sc ? "SC/RPA+HCI-Reset/S-13" : "legacy/RPA+HCI-Reset/S-13") : sc ? "SC" : "legacy",seed.rand_calls,d.heap_peak);
    return 0;
}
