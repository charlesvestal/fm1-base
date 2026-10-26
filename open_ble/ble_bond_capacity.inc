/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
/* FMOB v2, 144 bytes/identity, explicitly byte encoded (no ABI structs):
 * 0 address type, 1..6 address, 7 presence mask (OUR_SEC/PEER_SEC, CCCD0/1),
 * 8 MRU rank (1 newest), 9..12 GATT generation, 13..72 our security,
 * 73..132 peer security, 133..137 / 138..142 CCCDs, 143 reserved zero.
 * Security: size, flags, EDIV(2), Rand(8), LTK(16), IRK(16), CSRK(16).
 * CCCD: value handle(2), flags(2), value_changed(1).
 * No change-detection shadow: CRC of canonical live bytes vs saved CRC.
 * Inactive fields survive until flush for delete/recreate net-state no-ops. */
static struct {
    uint8_t peer[BLE_BOND_CAPACITY][BLE_BOND_PEER_SIZE];
    ble_bond_diag_t d;
    uint32_t saved_crc,saved_replacements;
} bonds __attribute__((section(".pool.ble")));
static int identity(ble_addr_t *a)
{
    if(a->type>3) return 0;
    a->type&=1u; return !a->type || ble_random_valid(a->val);
}
static int peer_find(const ble_addr_t *a)
{
    for(unsigned i=0;i<BLE_BOND_CAPACITY;i++)
        if((a->type&~2u)==bonds.peer[i][0] && !memcmp(a->val,bonds.peer[i]+1,6)) return (int)i;
    return -1;
}
static void reconcile(void)
{
    bonds.d.pending=ble_crc32(&bonds.peer[0][0],sizeof bonds.peer)!=bonds.saved_crc ||
        bonds.d.replacements!=bonds.saved_replacements;
}
static unsigned peer_count(void)
{
    unsigned n=0;
    for(unsigned i=0;i<BLE_BOND_CAPACITY;i++) n+=!!(bonds.peer[i][7]&3u);
    return n;
}
/* Close rank gaps without changing the order; at most eight identities. */
static void ranks(void)
{
    uint8_t old[BLE_BOND_CAPACITY];
    for(unsigned i=0;i<BLE_BOND_CAPACITY;i++) old[i]=bonds.peer[i][8];
    for(unsigned i=0;i<BLE_BOND_CAPACITY;i++) if(bonds.peer[i][7]&3u) {
        unsigned rank=1;
        for(unsigned j=0;j<BLE_BOND_CAPACITY;j++) if((bonds.peer[j][7]&3u) &&
            (old[j]<old[i] || (old[j]==old[i] && j<i))) rank++;
        bonds.peer[i][8]=(uint8_t)rank;
    }
}
void ble_bonds_touch(uint8_t type,const uint8_t addr[6])
{
    ble_addr_t a; a.type=type; memcpy(a.val,addr,6);
    int i=peer_find(&a);
    if(i<0 || !(bonds.peer[i][7]&3u) || bonds.peer[i][8]==1) return;
    unsigned old=bonds.peer[i][8];
    for(unsigned j=0;j<BLE_BOND_CAPACITY;j++)
        if((bonds.peer[j][7]&3u) && bonds.peer[j][8]<old) bonds.peer[j][8]++;
    bonds.peer[i][8]=1; ranks(); reconcile();
}
static int match(int type,const union ble_store_key *key,unsigned *part)
{
    if(type<1 || type>3) return -1;
    const ble_addr_t *a=type==3 ? &key->cccd.peer_addr : &key->sec.peer_addr;
    unsigned skip=type==3 ? key->cccd.idx : key->sec.idx;
    for(unsigned i=0;i<BLE_BOND_CAPACITY;i++) {
        uint8_t *p=bonds.peer[i];
        if(ble_addr_cmp(a,BLE_ADDR_ANY) && ((a->type&~2u)!=p[0] || memcmp(a->val,p+1,6))) continue;
        if(type<3) {
            if((p[7]&(1u<<(type-1))) && !skip--) { *part=13u+(unsigned)(type-1)*60u; return (int)i; }
        } else for(unsigned c=0;c<2;c++) if((p[7]&(4u<<c)) &&
            (!key->cccd.chr_val_handle || key->cccd.chr_val_handle==ble_get16(p+133+c*5u)) && !skip--) {
            *part=133+c*5u; return (int)i;
        }
    }
    return -1;
}
int ble_bonds_read(int type,const union ble_store_key *key,union ble_store_value *out)
{
    unsigned at; int i=match(type,key,&at);
    if(i<0) return BLE_HS_ENOENT;
    const uint8_t *p=bonds.peer[i],*s=p+at;
    memset(out,0,type==3 ? sizeof out->cccd : sizeof out->sec);
    if(type==3) {
        out->cccd.peer_addr.type=p[0]; memcpy(out->cccd.peer_addr.val,p+1,6);
        out->cccd.chr_val_handle=ble_get16(s); out->cccd.flags=ble_get16(s+2); out->cccd.value_changed=s[4]&1u;
    } else {
        out->sec.peer_addr.type=p[0]; memcpy(out->sec.peer_addr.val,p+1,6);
        out->sec.key_size=s[0]; out->sec.ediv=ble_get16(s+2);
        for(unsigned j=0;j<8;j++) out->sec.rand_num|=(uint64_t)s[4+j]<<(8u*j);
        memcpy(out->sec.ltk,s+12,16); memcpy(out->sec.irk,s+28,16); memcpy(out->sec.csrk,s+44,16);
        out->sec.ltk_present=s[1]&1u; out->sec.irk_present=(s[1]>>1)&1u;
        out->sec.csrk_present=(s[1]>>2)&1u; out->sec.authenticated=(s[1]>>3)&1u; out->sec.sc=(s[1]>>4)&1u;
    }
    return 0;
}
int ble_bonds_write(int type,const union ble_store_value *v)
{
    uint8_t data[60]={0}; ble_addr_t a; unsigned at,bit;
    if(type<1 || type>3) return BLE_HS_ENOTSUP;
    a=type==3 ? v->cccd.peer_addr : v->sec.peer_addr;
    if(!identity(&a)) { bonds.d.private_refused++; return BLE_HS_EINVAL; }
    if(type<3 && (v->sec.key_size<7 || v->sec.key_size>16)) return BLE_HS_EINVAL;
    if(type==3 && !v->cccd.chr_val_handle) return BLE_HS_EINVAL;
    int i=peer_find(&a);
    if(i<0) {
        for(unsigned j=0;j<BLE_BOND_CAPACITY;j++) if(!bonds.peer[j][7]) { i=(int)j; break; }
        if(i<0) return BLE_HS_ESTORE_CAP;
        memset(bonds.peer[i],0,BLE_BOND_PEER_SIZE); bonds.peer[i][0]=a.type; memcpy(bonds.peer[i]+1,a.val,6);
        ble_put32(bonds.peer[i]+9,BLE_GATT_DB_VERSION);
    }
    uint8_t *p=bonds.peer[i]; unsigned was_sec=p[7]&3u;
    if(type<3) {
        at=13u+(unsigned)(type-1)*60u; bit=1u<<(type-1);
        data[0]=v->sec.key_size; data[1]=(uint8_t)(v->sec.ltk_present | v->sec.irk_present<<1 |
            v->sec.csrk_present<<2 | v->sec.authenticated<<3 | v->sec.sc<<4);
        ble_put16(data+2,v->sec.ediv);
        for(unsigned j=0;j<8;j++) data[4+j]=(uint8_t)(v->sec.rand_num>>(8u*j));
        if(v->sec.ltk_present) memcpy(data+12,v->sec.ltk,16);
        if(v->sec.irk_present) memcpy(data+28,v->sec.irk,16);
        if(v->sec.csrk_present) memcpy(data+44,v->sec.csrk,16);
    } else {
        unsigned c;
        /* Retain the original CCCD slot during delete/recreation. */
        for(c=0;c<2;c++) if(ble_get16(p+133+c*5u)==v->cccd.chr_val_handle) break;
        if(c==2) for(c=0;c<2;c++) if(!(p[7]&(4u<<c))) break;
        if(c==2) return BLE_HS_ESTORE_CAP;
        at=133+c*5u; bit=4u<<c;
        ble_put16(data,v->cccd.chr_val_handle); ble_put16(data+2,v->cccd.flags); data[4]=!!v->cccd.value_changed;
    }
    unsigned n=type<3 ? 60u : 5u;
    if((p[7]&bit) && !memcmp(p+at,data,n)) { bonds.d.net_noops++; return 0; }
    /* Match R3/R4: a newly created OUR_SEC starts at the current schema,
     * including re-pairing a deleted identity whose RAM tombstone remains. */
    if((type==1 && !(p[7]&1u)) || (type==2 && !was_sec)) ble_put32(p+9,BLE_GATT_DB_VERSION);
    memcpy(p+at,data,n); p[7]|=(uint8_t)bit;
    if(type<3 && !was_sec) {
        if(!p[8]) p[8]=BLE_BOND_CAPACITY+1u;
        ble_bonds_touch(a.type,a.val);
    }
    reconcile(); return 0;
}
int ble_bonds_delete(int type,const union ble_store_key *key)
{
    unsigned at; int i=match(type,key,&at);
    if(i<0) return BLE_HS_ENOENT;
    unsigned bit=type<3 ? 1u<<(type-1) : 4u<<((at-133u)/5u);
    bonds.peer[i][7]&=(uint8_t)~bit; reconcile(); return 0;
}
int ble_bonds_db_version(uint8_t type,const uint8_t addr[6],uint32_t *version)
{
    ble_addr_t a; a.type=type; memcpy(a.val,addr,6); int i=peer_find(&a);
    if(i<0 || !(bonds.peer[i][7]&1u)) return BLE_HS_ENOENT;
    *version=ble_get32(bonds.peer[i]+9); return 0;
}
int ble_bonds_db_confirm(uint8_t type,const uint8_t addr[6],uint32_t version)
{
    ble_addr_t a; a.type=type; memcpy(a.val,addr,6); int i=peer_find(&a);
    if(i<0 || !(bonds.peer[i][7]&1u)) return BLE_HS_ENOENT;
    ble_put32(bonds.peer[i]+9,version); reconcile(); return 0;
}
void ble_bonds_replaced(void) { bonds.d.replacements++; reconcile(); }
void ble_bonds_get_diag(ble_bond_diag_t *out)
{
    *out=bonds.d; out->count=(uint8_t)peer_count(); out->capacity=BLE_BOND_CAPACITY; out->records=0;
    for(unsigned i=0;i<BLE_BOND_CAPACITY;i++) for(unsigned b=0;b<4;b++) out->records+=(bonds.peer[i][7]>>b)&1u;
}
int ble_bonds_peer(unsigned index,ble_bond_peer_diag_t *out)
{
    for(unsigned i=0;i<BLE_BOND_CAPACITY;i++) {
        uint8_t *p=bonds.peer[i];
        if(!(p[7]&3u)) continue;
        unsigned rank=1;
        for(unsigned j=0;j<BLE_BOND_CAPACITY;j++) if((bonds.peer[j][7]&3u) &&
            (bonds.peer[j][8]<p[8] || (bonds.peer[j][8]==p[8] && j<i))) rank++;
        if(rank!=index+1u) continue;
        memset(out,0,sizeof *out); out->type=p[0]; memcpy(out->addr,p+1,6); out->rank=(uint8_t)rank;
        out->sc=(p[(p[7]&1u) ? 14 : 74]>>4)&1u;
        for(unsigned c=0;c<2;c++) if(p[7]&(4u<<c)) {
            out->cccd_handle[c]=ble_get16(p+133+c*5u); out->cccd_flags[c]=ble_get16(p+135+c*5u);
        }
        return 0;
    }
    return BLE_HS_ENOENT;
}
static void canonicalize(void)
{
    for(unsigned i=0;i<BLE_BOND_CAPACITY;i++) {
        uint8_t *p=bonds.peer[i];
        if(!(p[7]&3u)) {
            if(p[7]&12u) bonds.d.orphan_dropped++;
            memset(p,0,BLE_BOND_PEER_SIZE); continue;
        }
        for(unsigned s=0;s<2;s++) if(!(p[7]&(1u<<s))) memset(p+13+s*60u,0,60);
        for(unsigned c=0;c<2;c++) if(!(p[7]&(4u<<c))) memset(p+133+c*5u,0,5);
    }
    ranks(); reconcile();
}
void ble_bonds_init(void)
{
    memset(&bonds,0,sizeof bonds);
    ble_object_bonds_load(&bonds.peer[0][0],&bonds.d.replacements);
    bonds.saved_replacements=bonds.d.replacements; bonds.saved_crc=ble_crc32(&bonds.peer[0][0],sizeof bonds.peer);
    for(unsigned i=0;i<BLE_BOND_CAPACITY;i++) {
        uint8_t *p=bonds.peer[i]; ble_addr_t a; a.type=p[0]; memcpy(a.val,p+1,6);
        if(!p[7]) continue;
        if(!identity(&a)) { memset(p,0,BLE_BOND_PEER_SIZE); bonds.d.private_dropped++; continue; }
        int bad=p[0]>1 || p[7]>15 || p[143] || ((p[7]&3u) && (!p[8] || p[8]>BLE_BOND_CAPACITY));
        for(unsigned s=0;s<2;s++) if(p[7]&(1u<<s)) {
            const uint8_t *v=p+13+s*60u; bad|=v[0]<7 || v[0]>16 || v[1]>31;
        }
        for(unsigned c=0;c<2;c++) if(p[7]&(4u<<c)) bad|=!ble_get16(p+133+c*5u) || p[137+c*5u]>1;
        for(unsigned j=0;j<i;j++) if(bonds.peer[j][7] && !memcmp(p,bonds.peer[j],7)) bad=1;
        if(bad) { memset(p,0,BLE_BOND_PEER_SIZE); bonds.d.malformed++; }
    }
    canonicalize();
}
void ble_bonds_flush(void)
{
    if(!bonds.d.pending || !ble_port_storage_idle()) return;
    canonicalize(); if(!bonds.d.pending) return;
    if(ble_object_bonds_save(&bonds.peer[0][0],bonds.d.replacements)) { bonds.d.failures++; return; }
    bonds.saved_crc=ble_crc32(&bonds.peer[0][0],sizeof bonds.peer); bonds.saved_replacements=bonds.d.replacements;
    bonds.d.writes++; bonds.d.erases++; reconcile();
}
/* R1--R4 FLBD v1: no recency was recorded. Establish a deterministic initial
 * order by first valid record slot (later slots newer), retaining both directions,
 * cache generations and both CCCDs. This helper runs only during object load. */
void ble_bonds_migrate(const uint8_t *old,uint8_t *peers)
{
    memset(peers,0,BLE_BOND_BYTES);
    for(unsigned slot=0;slot<BLE_BOND_SLOTS;slot++) {
        const uint8_t *r=old+slot*128u,*v=r+12;
        if(memcmp(r,"FLBD",4) || r[4]!=1 || r[5]<1 || r[5]>3 || r[6]>=BLE_BOND_SLOTS || r[7]!=1 ||
           ble_get32(r+124)!=ble_crc32(r,124)) continue;
        int bad=0; for(unsigned n=92;n<124;n++) bad|=r[n];
        ble_addr_t a; a.type=v[0]; memcpy(a.val,v+1,6);
        if(bad || !identity(&a) || (r[5]<3 && (v[7]<7 || v[7]>16 || v[66]>31))) continue;
        unsigned i;
        for(i=0;i<BLE_BOND_CAPACITY;i++) if(peers[i*BLE_BOND_PEER_SIZE+7] &&
            peers[i*BLE_BOND_PEER_SIZE]==a.type && !memcmp(peers+i*BLE_BOND_PEER_SIZE+1,a.val,6)) break;
        if(i==BLE_BOND_CAPACITY) for(i=0;i<BLE_BOND_CAPACITY;i++) if(!peers[i*BLE_BOND_PEER_SIZE+7]) break;
        if(i==BLE_BOND_CAPACITY) continue;
        uint8_t *p=peers+i*BLE_BOND_PEER_SIZE; p[0]=a.type; memcpy(p+1,a.val,6);
        if(r[5]<3) {
            unsigned s=r[5]-1u; uint8_t *d=p+13+s*60u;
            if(!(p[7]&3u)) p[8]=(uint8_t)(BLE_BOND_CAPACITY-i);
            if(!s || !(p[7]&1u)) memcpy(p+9,v+68,4);
            p[7]|=(uint8_t)(1u<<s); d[0]=v[7]; d[1]=v[66]; memcpy(d+2,v+8,10);
            if(v[66]&1u) memcpy(d+12,v+18,16);
            if(v[66]&2u) memcpy(d+28,v+34,16);
            if(v[66]&4u) memcpy(d+44,v+50,16);
        } else {
            unsigned c; for(c=0;c<2;c++) if(!(p[7]&(4u<<c))) break;
            if(c<2 && ble_get16(v+8)) { p[7]|=(uint8_t)(4u<<c); memcpy(p+133+c*5u,v+8,5); p[137+c*5u]&=1u; }
        }
    }
    for(unsigned i=0;i<BLE_BOND_CAPACITY;i++) {
        uint8_t *p=peers+i*BLE_BOND_PEER_SIZE;
        if(!(p[7]&3u)) { memset(p,0,BLE_BOND_PEER_SIZE); continue; }
        unsigned rank=1;
        /* The entry index supplies the legacy order, independent of CCCD slot order. */
        for(unsigned j=i+1;j<BLE_BOND_CAPACITY;j++) if(peers[j*BLE_BOND_PEER_SIZE+7]&3u) rank++;
        p[8]=(uint8_t)rank;
    }
}
