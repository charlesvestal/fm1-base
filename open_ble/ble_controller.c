/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* In-memory H4 controller, r4 03 and 04 §10. LL executor context.
 * Host callbacks only copy into the bounded host transport ring. */
#include "ble_s1.h"
#include "ble_link.h"
#include "ble_memory.h"
#include "ble_timing.h"
#if FELUCCA_BLE_STAGE >= 3
#include "ble_security.h"
#include "ble_seed.h"
#include "ble_crypto.h"
#if FELUCCA_BLE_S31
#include "ble_privacy.h"
#endif
#endif

typedef struct { uint8_t data[BLE_HCI_MAX], cancel; uint16_t n;
#if FM1B_BLE_RELIABILITY
    uint32_t epoch;
#endif
} command_t;
static struct {
    command_t q[4];
    unsigned r,w,count;
    ble_host_fn host;
    uint8_t pub[6],random[6],random_set,masks[2][8],random_pending,start_pending,reset_pending;
    ble_adv_t adv;
    ble_diag_t diag;
    uint16_t deferred;
    uint8_t reply[70],overflow_reply[6];
    uint16_t overflow_op[128]; uint8_t overflow_r,overflow_w,overflow_count;
} ctl;
#if FM1B_BLE_RELIABILITY
uint32_t ble_reliability_old_commands(int all)
{
    uint32_t mask=0;
    for(unsigned i=0;i<ctl.count;i++) {
        const command_t *c=&ctl.q[(ctl.r+i)%4u];
        if(!all && c->epoch==ble_link.epoch) continue;
        mask|=128u;
        uint16_t op=ble_get16(c->data);
        if(op==0x201a || op==0x201b) mask|=256u;
    }
    return mask;
}
#endif
_Static_assert(sizeof(ble_trace_t)==40,"trace wire/storage layout");
#if FM1B_BLE_DIAG
static ble_trace_t trace[BLE_TRACE_N] __attribute__((section(".ble_trace"),aligned(4)));
static uint32_t trace_w,trace_used;
static uint8_t trace_frozen;
#endif
static struct { uint8_t active,waiting,type,own_type,random[6]; uint16_t expected; } driver;

static void trace_add(uint8_t type,const uint8_t *p,uint16_t n,uint8_t status)
{
#if FM1B_BLE_DIAG
    ble_trace_t *r;
    if (trace_frozen) { ctl.diag.trace_frozen_drops++; return; }
    r=&trace[trace_w++%BLE_TRACE_N];
    r->ms=ble_port_ms(); r->len=n; r->dir_type=type; r->intake=status;
    memset(r->data,0,sizeof r->data); memcpy(r->data,p,n<32 ? n : 32);
#if FELUCCA_BLE_STAGE >= 3
    /* Console traces retain framing/counters, never SMP keys or Rand output. */
    if((type==129 && n>=3 && (ble_get16(p)==0x201a || ble_get16(p)==0x2017)))
        memset(r->data+3,0,29);
    if(type==129 && n>=10 && ble_get16(p)==0x2027) memset(r->data+10,0,22);
    if((type==2 || type==130) && n>=4) memset(r->data+4,0,28);
    if(type==4 && n>=6 && p[0]==14 && (ble_get16(p+3)==0x2017 || ble_get16(p+3)==0x2018))
        memset(r->data+6,0,26);
#endif
    if (trace_used<BLE_TRACE_N) trace_used++;
    ctl.diag.trace_count++;
#else
    (void)type; (void)p; (void)n; (void)status;
#endif
}
static void driver_receive(uint8_t type,const uint8_t *p,uint16_t n)
{
    if (type!=4 || n<6 || p[0]!=14 || !driver.waiting) return;
    if (ble_get16(p+3)!=driver.expected || p[5]) {
        ctl.diag.driver_error=1; driver.active=0;
    } else ctl.diag.driver_step++;
    driver.waiting=0;
}
static void complete(uint16_t opcode,uint8_t status,const uint8_t *data,uint8_t n,int immediate)
{
    uint8_t *b=immediate ? ctl.overflow_reply : ctl.reply;
    /* All error replies contain status only. No uninitialised success payload. */
    if (status && (immediate || (opcode!=0x201a && opcode!=0x201b))) n=0;
    b[0]=14; b[1]=(uint8_t)(4+n); b[2]=1;
    ble_put16(b+3,opcode); b[5]=status;
    if (n) memcpy(b+6,data,n);
    BLE_DIAG_ONLY(ctl.diag.replies++); ctl.diag.last_opcode=opcode; ctl.diag.last_status=status;
    trace_add(4,b,(uint16_t)(6+n),0);
    if (ctl.host) ctl.host(4,b,(uint16_t)(6+n));
}
static void defaults(void)
{
    memset(&ctl.adv,0,sizeof ctl.adv);
    ctl.adv.params.min=ctl.adv.params.max=2048;
    ctl.adv.params.channels=7; ctl.adv.power=6;
    memset(ctl.random,0,6); ctl.random_set=0;
    memset(ctl.masks,0,sizeof ctl.masks);
    memset(ctl.masks[0],255,5); ctl.masks[0][5]=31;
    ctl.masks[1][0]=31;
    ctl.diag.enabled=0; ctl.diag.stopping=0;
}
void ble_init(const uint8_t *public_addr,ble_host_fn host)
{
    memset(&ctl,0,sizeof ctl); memset(&driver,0,sizeof driver);
    BLE_DIAG_ONLY(trace_w=trace_used=0; trace_frozen=0);
#if FELUCCA_BLE_S31
    ble_privacy_reset();
#endif
    defaults(); ble_ll_init();
    if (ble_public_valid(public_addr)) memcpy(ctl.pub,public_addr,6);
    ctl.host=host;
}
void ble_set_host(ble_host_fn host) { driver.active=driver.waiting=0; ctl.host=host; }
int ble_hci_send(uint8_t type,const uint8_t *packet,uint16_t n)
{
    uint32_t key;
    if(type==2 && packet) {
        int rc=ble_ll_acl(packet,n); trace_add(130,packet,n,(uint8_t)rc); return rc;
    }
    if (!packet || type!=1 || n<3 || n>BLE_HCI_MAX || n!=3u+packet[2]) {
        ctl.diag.rejected++;
        if (packet) trace_add((uint8_t)(128u|type),packet,n,1);
        return 1;
    }
    key=ble_port_lock();
    BLE_DIAG_ONLY(if (!ctl.diag.commands && !ctl.diag.replies) ctl.diag.first_command_ticks=ble_port_ticks());
    trace_add(129,packet,n,0);
    if (ctl.count==4) {
        ctl.diag.overflow++;
        if(ctl.overflow_count==128) { ctl.diag.rejected++; ble_port_unlock(key); return 1; }
        ctl.overflow_op[ctl.overflow_w++%128u]=ble_get16(packet); ctl.overflow_count++;
        ble_port_unlock(key); ble_port_pend(); return 0; /* answered, not enqueued; exactly one response */
    }
    if(ble_get16(packet)==0x0c03 && n==3) {
        unsigned i;
        for(i=0;i<ctl.count;i++) ctl.q[(ctl.r+i)%4].cancel=1;
    }
    #if FM1B_BLE_RELIABILITY
    ctl.q[ctl.w].epoch=ble_link.epoch;
#endif
    memcpy(ctl.q[ctl.w].data,packet,n); ctl.q[ctl.w].n=n; ctl.q[ctl.w].cancel=0;
    ctl.w=(ctl.w+1u)%4; ctl.count++; BLE_DIAG_ONLY(ctl.diag.commands++);
    ble_port_unlock(key); ble_port_pend(); return 0;
}
static int command_length(uint16_t op)
{
    switch (op) {
    case 0x0c03: case 0x1001: case 0x1002: case 0x1003: case 0x1009:
    case 0x2002: case 0x2003: case 0x201c: return 0;
#if FELUCCA_BLE_STAGE >= 3
    case 0x2018: return 0;
    case 0x201b: return 2;
    case 0x201a: return 18;
    case 0x2017: return 32;
#if FELUCCA_BLE_S31
    case 0x2027: return 39;
    case 0x2028: return 7;
    case 0x204e: return 8;
    case 0x2029: case 0x202a: return 0;
    case 0x202d: return 1;
#endif
#endif
    case 0x2016: return 2;
    case 0x0406: return 3;
    case 0x0c01: case 0x2001: return 8;
    case 0x2005: return 6;
    case 0x2006: return 15;
    case 0x2008: case 0x2009: return 32;
    case 0x200a: return 1;
#if FELUCCA_BLE_TX_DBM != 127
    case 0x2007: return 0;
#endif
    default: return -1;
    }
}
static void supported(uint8_t *out)
{
    memset(out,0,64);
    out[5]=192; out[14]=40; out[15]=2;
    out[0]=0x20; out[25]=0xb7; out[26]=3; out[27]=0x20; out[28]=0x08;
#if FELUCCA_BLE_STAGE >= 3
    out[27]|=0xc0; out[28]|=0x06;
#if FELUCCA_BLE_S31
    out[34]=0x78; out[35]=2; out[39]=4;
#endif
#endif
#if FELUCCA_BLE_TX_DBM != 127
    out[25]|=64;
#endif
}
static void reset_defaults(void)
{
    uint8_t power=ctl.adv.power;
#if FM1B_BLE_DIAG
    uint32_t first=ctl.diag.first_command_ticks,trace_count=ctl.diag.trace_count,drops=ctl.diag.trace_frozen_drops;
#endif
    memset(&ctl.diag,0,sizeof ctl.diag);
    BLE_DIAG_ONLY(ctl.diag.first_command_ticks=first; ctl.diag.trace_count=trace_count; ctl.diag.trace_frozen_drops=drops);
    defaults(); ctl.adv.power=power;
}
static void reset_step(void)
{
    static unsigned privacy_offset; (void)privacy_offset;
    uint32_t item=ble_time_item_begin(); ble_work_kind_t kind=BLE_WORK_RESET_CLEAR;
    switch(ctl.reset_pending) {
    case 1:
        /* Clear timing first, so the later Reset slices remain observable. */
        kind=BLE_WORK_RESET_STATS; ble_port_reset_stats();
#if FELUCCA_BLE_STAGE >= 3
        ble_seed_reset_stats();
#endif
        ctl.reset_pending=2; break;
    case 2: {
        int rc=ble_port_stop_poll(); kind=BLE_WORK_STOP;
        if(rc<0) { ctl.reset_pending=0; complete(0x0c03,3,0,0,0); break; }
        if(rc>0) ctl.reset_pending=3;
        break;
    }
    case 3: if(ble_ll_reset_step()) { privacy_offset=0; ctl.reset_pending=4; } break;
    case 4:
#if FELUCCA_BLE_S31
        if(!ble_privacy_reset_step(privacy_offset)) { privacy_offset+=128u; break; }
#endif
        ctl.reset_pending=5; break;
    case 5: kind=BLE_WORK_RESET_DEFAULTS; reset_defaults(); ctl.reset_pending=6; break;
    case 6:
        kind=BLE_WORK_RESET_COMPLETE; ctl.reset_pending=0; complete(0x0c03,0,0,0,0); break;
    }
    ble_time_item_end(item,kind,0x0c03);
    if(ctl.reset_pending || ctl.count || ctl.overflow_count) ble_port_pend();
}
static void command_status(uint16_t opcode,uint8_t status)
{
    uint8_t b[6]={15,4,status,1,0,0}; ble_put16(b+4,opcode);
    BLE_DIAG_ONLY(ctl.diag.replies++); ctl.diag.last_opcode=opcode; ctl.diag.last_status=status;
    trace_add(4,b,6,0); if(ctl.host) ctl.host(4,b,6);
}
static void execute(const command_t *cmd)
{
    uint16_t op=ble_get16(cmd->data);
    const uint8_t *p=cmd->data+3;
    uint8_t data[64],n=0,status=0;
    int expected=command_length(op);
    if (cmd->cancel) status=0x0c;
    else if (expected<0) status=1;
    else if (cmd->data[2]!=expected) status=0x12;
    else switch (op) {
    case 0x0c03: {
        uint32_t k=ble_port_lock(); ble_ll_reset_begin(); ble_port_unlock(k);
        ctl.reset_pending=1; ctl.diag.stopping=1; ble_port_stop_begin(); ble_port_pend(); return;
    }
    case 0x1001:
        data[0]=6; ble_put16(data+1,1); data[3]=6;
        ble_put16(data+4,0xffff); ble_put16(data+6,1); n=8; break;
    case 0x1002: supported(data); n=64; break;
    case 0x1003: memset(data,0,8); data[4]=0x60; n=8; break;
    case 0x1009: memcpy(data,ctl.pub,6); n=6; break;
    case 0x0c01: memcpy(ctl.masks[0],p,8); break;
    case 0x2001: memcpy(ctl.masks[1],p,8); break;
    case 0x2002: ble_put16(data,27); data[2]=5; n=3; break;
    case 0x2003: memset(data,0,8); data[0]=FELUCCA_BLE_STAGE>=3 ? 1 : 0; n=8; break; /* S2 has no encryption */
    case 0x201c: memset(data,0,8); data[0]=0x85; n=8; break;
#if FELUCCA_BLE_STAGE >= 3
    case 0x2017: if(!ble_aes_le(p,p+16,data)) status=3; else n=16; break;
    case 0x2018:
        ble_seed_rand_called(); ctl.random_pending=1; ble_port_pend(); return;
#if FELUCCA_BLE_S31
    case 0x202a: data[0]=BLE_RL_CAPACITY; n=1; break;
    case 0x2027: case 0x2028: case 0x2029: case 0x202d: case 0x204e: {
        uint32_t k=ble_port_lock(); status=ble_privacy_command(op,p,ctl.diag.enabled);
        ble_port_unlock(k); break;
    }
#endif
#else
    case 0x2018: status=3; break;
#endif /* S2 entropy service disabled, no fabricated randomness */
    case 0x201a: case 0x201b:
#if FELUCCA_BLE_STAGE >= 3
        status=ble_ll_ltk_reply(ble_get16(p),op==0x201a ? p+2 : 0,op==0x201b); break;
#else
        status=ble_get16(p)!=BLE_HANDLE || ble_link.state==BLE_LINK_IDLE ? 2 : 0x0c; break;
#endif
    case 0x2016: {
        uint32_t k=ble_port_lock(); status=ble_ll_features(ble_get16(p)); ble_port_unlock(k); break;
    }
    case 0x0406: {
        uint32_t k=ble_port_lock(); status=ble_ll_disconnect(ble_get16(p),p[2]); ble_port_unlock(k); break;
    }
    case 0x2005:
        if (ctl.diag.enabled || ble_link.state!=BLE_LINK_IDLE) status=0x0c;
        else if (!ble_random_valid(p)) status=0x12;
        else { memcpy(ctl.random,p,6); ctl.random_set=1; }
        break;
    case 0x2006:
        status=ctl.diag.enabled ? 0x0c : ble_adv_validate(p,&ctl.adv.params); break;
    case 0x2008: case 0x2009:
        if (p[0]>31) status=0x12;
        else {
            uint8_t *dst=op==0x2008 ? ctl.adv.data : ctl.adv.scan;
            memset(dst,0,31); memcpy(dst,p+1,p[0]);
            if (op==0x2008) ctl.adv.data_len=p[0]; else ctl.adv.scan_len=p[0];
            if (ctl.diag.enabled) ble_port_update(&ctl.adv);
        }
        break;
    case 0x200a:
        if (p[0]>1) status=0x12;
        else if (!p[0]) {
            if (ctl.diag.enabled && ble_link.state==BLE_LINK_IDLE) {
                ctl.deferred=op; ctl.diag.stopping=1; ble_port_stop_begin(); return;
            }
        } else if (ble_link.state!=BLE_LINK_IDLE) status=0x0c;
        else if (ble_port_failed()) status=3;
        else if (!ctl.diag.enabled) {
            const uint8_t *addr=ctl.adv.params.own_type ? ctl.random : ctl.pub;
            if (ctl.adv.params.own_type ? !ctl.random_set : !ble_public_valid(addr)) status=0x12;
            else if (ble_port_failed()) status=3;
            else {
                memcpy(ctl.adv.addr,addr,6);
                int rc=ble_port_start(&ctl.adv);
                if(rc>0) { ctl.deferred=op; ctl.start_pending=1; ble_port_pend(); return; }
                status=rc<0 ? 3 : 0;
                if (!status) ctl.diag.enabled=1;
            }
        }
        break;
#if FELUCCA_BLE_TX_DBM != 127
    case 0x2007:
        if (ctl.adv.power!=6) status=3;
        else { data[0]=(uint8_t)FELUCCA_BLE_TX_DBM; n=1; }
        break;
#endif
    default: status=1; break;
    }
    if((op==0x2016 || op==0x0406) && expected>=0) command_status(op,status);
    else {
        if(expected>=0 && (op==0x201a || op==0x201b)) { ble_put16(data,cmd->data[2]>=2 ? ble_get16(p) : 0); n=2; }
        complete(op,status,data,n,0);
    }
}
static void driver_pass(void)
{
    static const uint16_t ops[]={0x0c03,0x1001,0x1002,0x1003,0x0c01,0x2001,
                                0x2002,0x2003,0x1009,0x2005,0x2008,0x2009,0x2006,0x200a};
    static const uint8_t mask[8]={0x90,0x80,0,2,0,0x80,0,0x20};
    uint8_t cmd[35]={0},step=ctl.diag.driver_step;
    uint16_t op;
    if (!driver.active || driver.waiting || ctl.count || ctl.deferred) return;
    if (step==9 && !driver.own_type) step=++ctl.diag.driver_step;
    if (step>=sizeof ops/sizeof ops[0]) { driver.active=0; return; }
    op=ops[step]; ble_put16(cmd,op);
    if (step==4) { cmd[2]=8; memcpy(cmd+3,mask,8); }
    if (step==5) { cmd[2]=8; cmd[3]=31; }
    if (step==9) { cmd[2]=6; memcpy(cmd+3,driver.random,6); }
    if (step==10) {
        cmd[2]=32; cmd[3]=3; cmd[4]=2; cmd[5]=1; cmd[6]=6;
        /* NONCONN has no scan response: repeat complete name in ADV data. */
        if (driver.type==3) { cmd[3]=16; cmd[7]=12; cmd[8]=9; memcpy(cmd+9,"Felucca BLE",11); }
    }
    if (step==11) { cmd[2]=32; cmd[3]=13; cmd[4]=12; cmd[5]=9; memcpy(cmd+6,"Felucca BLE",11); }
    if (step==12) {
        cmd[2]=15; ble_put16(cmd+3,160); ble_put16(cmd+5,160);
        cmd[7]=driver.type; cmd[8]=driver.own_type; cmd[16]=7;
    }
    if (step==13) { cmd[2]=1; cmd[3]=1; }
    driver.waiting=1; driver.expected=op;
    if (ble_hci_send(1,cmd,(uint16_t)(3+cmd[2]))) {
        driver.waiting=driver.active=0; ctl.diag.driver_error=2;
    }
}
static void link_deliver(void)
{
    uint8_t b[40],type; uint16_t n; unsigned limit=1; uint32_t start=ble_time_own();
    while(limit-- && (uint32_t)(ble_time_own()-start)<150u*24u && ble_ll_poll(&type,b,&n)) {
        int deliver=1;
        if(type==4 && b[0]==0x3e && b[2]==1) ctl.diag.enabled=0;
#if FELUCCA_BLE_S31
        if(type==4 && b[0]==0x3e && b[2]==1 && (ctl.masks[1][1]&2u)) {
            memcpy(b+26,b+14,7); memset(b+14,0,12);
            if(ble_link.peer_resolved) {
                b[7]=(uint8_t)(ble_link.peer_identity_type+2u);
                memcpy(b+8,ble_link.peer_identity,6); memcpy(b+20,ble_link.conn.peer,6);
            }
            b[1]=31; b[2]=10; n=33;
        }
#endif
        if(type==4 && b[0]!=0x13) {
            unsigned bit=b[0]-1u;
            deliver=(ctl.masks[0][bit/8]>>(bit%8))&1u;
            if(b[0]==0x3e) { bit=b[2]-1u; deliver=deliver && ((ctl.masks[1][bit/8]>>(bit%8))&1u); }
        }
#if FELUCCA_BLE_STAGE >= 3
        if(!deliver && type==4 && b[0]==0x3e && b[2]==5) (void)ble_ll_ltk_reply(BLE_HANDLE,0,1);
#endif
        trace_add(type,b,n,0); if(deliver && ctl.host) ctl.host(type,b,n);
    }
}
void ble_worker(void)
{
    uint32_t item;
    if(!ble_time_budget()) { ble_port_pend(); return; }
    if(ctl.reset_pending) { reset_step(); return; }
#if FELUCCA_BLE_STAGE >= 3
    ble_entropy_step();
    if(!ble_time_budget()) { ble_port_pend(); return; }
    if(ctl.random_pending) {
        uint8_t random[8]; int rc;
        item=ble_time_item_begin(); rc=ble_entropy_request(BLE_RANDOM_HCI,random,sizeof random);
        if(rc) { ctl.random_pending=0; complete(0x2018,rc<0 ? 3 : 0,random,rc<0 ? 0 : sizeof random,0); }
        ble_secure_zero(random,sizeof random); ble_time_item_end(item,BLE_WORK_COMMAND,0x2018);
        if(!rc || ctl.count || ctl.overflow_count) ble_port_pend();
        return;
    }
#endif
    if (ctl.deferred) {
        if(ctl.start_pending) {
            int rc; item=ble_time_item_begin(); rc=ble_port_start(&ctl.adv);
            if(rc<=0) {
                ctl.start_pending=0; ctl.deferred=0;
                if(!rc) ctl.diag.enabled=1;
                complete(0x200a,rc<0 ? 3 : 0,0,0,0);
            }
            if(rc>0 || ctl.count || ctl.overflow_count) ble_port_pend();
            ble_time_item_end(item,BLE_WORK_ADV_PREP,0x200a); return;
        }
        int rc; item=ble_time_item_begin(); rc=ble_port_stop_poll();
        uint16_t op;
        if (!rc) { ble_port_pend(); ble_time_item_end(item,BLE_WORK_STOP,ctl.deferred); return; }
        op=ctl.deferred; ctl.deferred=0; ctl.diag.stopping=0; ctl.diag.enabled=0;
        complete(op,rc<0 ? 3 : 0,0,0,0);
        if(ctl.count || ctl.overflow_count) ble_port_pend();
        ble_time_item_end(item,BLE_WORK_STOP,op);
        return;
    }
    if(ctl.overflow_count) {
        item=ble_time_item_begin();
        uint32_t key=ble_port_lock(); uint16_t op=ctl.overflow_op[ctl.overflow_r++%128u];
        ctl.overflow_count--; ble_port_unlock(key); complete(op,0x0c,0,0,1);
        ble_time_item_end(item,BLE_WORK_COMMAND,op);
    }
    if(ble_time_budget()) {
        ble_work_kind_t kind=BLE_WORK_DELIVERY;
#if FELUCCA_BLE_S31
        uint32_t epoch=ble_link.epoch; uint8_t index=ble_link.resolve_index;
        if(ble_link.created_pending && !ble_link.resolve_done) kind=BLE_WORK_RESOLUTION;
#endif
        item=ble_time_item_begin(); link_deliver();
#if FELUCCA_BLE_S31
        /* Include an acceptance interrupt immediately before the poll lock. */
        if(index!=ble_link.resolve_index ||
           (epoch!=ble_link.epoch && ble_link.created_pending && ble_link.resolve_index)) kind=BLE_WORK_RESOLUTION;
#endif
        ble_time_item_end(item,kind,0);
    }
    if(ble_time_budget()) { item=ble_time_item_begin(); driver_pass(); ble_time_item_end(item,BLE_WORK_DRIVER,0); }
    if (ctl.count && ble_time_budget()) {
        item=ble_time_item_begin();
        uint32_t key=ble_port_lock();
        command_t cmd=ctl.q[ctl.r];
        ctl.r=(ctl.r+1u)%4; ctl.count--; ble_port_unlock(key);
        execute(&cmd);
        ble_time_item_end(item,ctl.start_pending ? BLE_WORK_ADV_PREP : BLE_WORK_COMMAND,ble_get16(cmd.data));
    }
    if(ctl.count || ctl.overflow_count || ctl.deferred || ctl.reset_pending || ble_ll_pending()) ble_port_pend();
}
void ble_driver_start(uint8_t type,const uint8_t *random_addr)
{
    memset(&driver,0,sizeof driver); driver.active=1; driver.type=type;
    ctl.diag.driver_step=ctl.diag.driver_error=0; ctl.host=driver_receive;
    driver.own_type=(uint8_t)!ble_public_valid(ctl.pub); ble_port_pend();
    if (random_addr) memcpy(driver.random,random_addr,6);
}
void ble_driver_stop(void) { driver.active=driver.waiting=0; }
void ble_get_diag(ble_diag_t *out) { *out=ctl.diag; }
void ble_trace_freeze(int freeze) { (void)freeze; BLE_DIAG_ONLY(trace_frozen=(uint8_t)!!freeze); }
uint32_t ble_trace_size(void) {
#if FM1B_BLE_DIAG
    return trace_used;
#else
    return 0;
#endif
}
const ble_trace_t *ble_trace_record(uint32_t oldest_index)
{
#if FM1B_BLE_DIAG
    if (!trace_frozen || oldest_index>=trace_used) return 0;
    return &trace[(trace_w-trace_used+oldest_index)%BLE_TRACE_N];
#else
    (void)oldest_index; return 0;
#endif
}
void ble_set_power(uint8_t level)
{
    ctl.adv.power=level>11 ? 11 : level;
    if (ctl.diag.enabled) ble_port_update(&ctl.adv);
}
