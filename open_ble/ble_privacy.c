/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* r8 03 §3.6. Peer privacy only (D-6); no local RPA generation. */
#include "ble_privacy.h"
#include "ble_crypto.h"
#include "ble_memory.h"
#if FELUCCA_BLE_S31
static struct {
    struct { uint8_t type,addr[6],peer_irk[16],local_irk[16],mode; } entry[BLE_RL_CAPACITY];
    ble_privacy_diag_t d;
} rl __attribute__((section(".pool.ble")));
static int nonzero(const uint8_t *p) { unsigned i; uint8_t v=0; for(i=0;i<16;i++) v|=p[i]; return v!=0; }
static int identity(const uint8_t *p) { return p[0]<=1 && (!p[0] || ble_random_valid(p+1)); }
static int find(const uint8_t *p)
{
    unsigned i;
    for(i=0;i<rl.d.count;i++) if(rl.entry[i].type==p[0] && !memcmp(rl.entry[i].addr,p+1,6)) return (int)i;
    return -1;
}
void ble_privacy_reset(void) { ble_secure_zero(&rl,sizeof rl); }
int ble_privacy_reset_step(unsigned offset)
{
    if(!offset) rl.d.enabled=rl.d.count=0;
    if(offset>=sizeof rl) return 1;
    unsigned n=sizeof rl-offset; if(n>128u) n=128u;
    ble_secure_zero((uint8_t *)&rl+offset,n); return offset+n==sizeof rl;
}
uint8_t ble_privacy_command(uint16_t op,const uint8_t *p,int advertising)
{
    int i; unsigned j;
    if((op==0x2027 || op==0x2028 || op==0x204e) && !identity(p)) return 0x12;
    if((op==0x202d && p[0]>1) || (op==0x204e && p[7]>1)) return 0x12;
    if((advertising || ble_link.created_pending) && (rl.d.enabled || op==0x202d)) return 0x0c;
    switch(op) {
    case 0x2027:
        if(find(p)>=0) return 0x12;
        for(j=0;j<rl.d.count;j++) if(nonzero(p+7) && !memcmp(p+7,rl.entry[j].peer_irk,16)) return 0x12;
        if(rl.d.count==BLE_RL_CAPACITY) return 7;
        i=rl.d.count++; rl.entry[i].type=p[0]; memcpy(rl.entry[i].addr,p+1,6);
        memcpy(rl.entry[i].peer_irk,p+7,16); memcpy(rl.entry[i].local_irk,p+23,16); rl.entry[i].mode=0;
        break;
    case 0x2028:
        i=find(p); if(i<0) return 2;
        for(j=(unsigned)i;j+1u<rl.d.count;j++) rl.entry[j]=rl.entry[j+1u];
        ble_secure_zero(&rl.entry[--rl.d.count],sizeof rl.entry[0]); break;
    case 0x2029: ble_secure_zero(rl.entry,sizeof rl.entry); rl.d.count=0; break;
    case 0x202d: rl.d.enabled=p[0]; break;
    case 0x204e: i=find(p); if(i<0) return 2; rl.entry[i].mode=p[7]; break;
    default: return 1;
    }
    return 0;
}
int ble_privacy_accept(const ble_conn_t *c)
{
    unsigned i;
    if(!rl.d.enabled) return 1;
    for(i=0;i<rl.d.count;i++) if(!rl.entry[i].mode && nonzero(rl.entry[i].peer_irk) &&
        c->peer_type==rl.entry[i].type && !memcmp(c->peer,rl.entry[i].addr,6)) {
        rl.d.network_rejected++; return 0;
    }
    return 1;
}
int ble_privacy_resolve_step(void)
{
    /* Keep the R4 one-entry quantum as the list grows to ten. In particular,
     * never turn this into a whole-list AES loop in an executor item. */
    unsigned i=ble_link.resolve_index++;
    uint8_t hash[3];
    if(ble_link.resolve_done) return 1;
    if(!i) {
        memcpy(ble_link.peer_identity,ble_link.conn.peer,6); ble_link.peer_identity_type=ble_link.conn.peer_type;
        if(!rl.d.enabled || ble_link.conn.peer_type!=1 || (ble_link.conn.peer[5]&0xc0u)!=0x40u) goto done;
        rl.d.attempts++;
    }
    if(i<rl.d.count) {
        if(nonzero(rl.entry[i].peer_irk)) {
            if(!ble_ah(rl.entry[i].peer_irk,ble_link.conn.peer+3,hash)) { rl.d.aes_failures++; goto miss; }
            if(!memcmp(hash,ble_link.conn.peer,3)) {
                memcpy(ble_link.peer_identity,rl.entry[i].addr,6); ble_link.peer_identity_type=rl.entry[i].type;
                ble_link.peer_resolved=1; rl.d.matches++; goto done;
            }
        }
        if(i+1u<rl.d.count) return 0;
    }
miss:
    rl.d.unresolved++;
done:
    ble_link.resolve_done=1; return 1;
}
void ble_privacy_get_diag(ble_privacy_diag_t *out)
{
    uint32_t k=ble_port_lock(); *out=rl.d; out->capacity=BLE_RL_CAPACITY; ble_port_unlock(k);
}
#endif
