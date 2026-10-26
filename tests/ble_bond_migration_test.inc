/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
static void r4_fixture(uint8_t old[BLE_LEGACY_OBJECT_SIZE])
{
    FILE *f=fopen("tests/fixtures/open_ble/r4-bonds-object.bin","rb"); assert(f);
    assert(fread(old,1,BLE_LEGACY_OBJECT_SIZE,f)==BLE_LEGACY_OBJECT_SIZE && fgetc(f)==EOF); fclose(f);
}
static void fixture_check(const uint8_t old[BLE_LEGACY_OBJECT_SIZE])
{
    for(unsigned peer=0;peer<2;peer++) {
        for(unsigned type=1;type<=2;type++) {
            const uint8_t *p=old+80+(peer*4u+type-1u)*128u+12;
            union ble_store_key k={0}; union ble_store_value v; k.sec.peer_addr.type=p[0]; memcpy(k.sec.peer_addr.val,p+1,6);
            assert(!ble_bonds_read((int)type,&k,&v));
            assert(v.sec.key_size==p[7] && v.sec.ediv==ble_get16(p+8));
            uint64_t rand=0; for(unsigned j=0;j<8;j++) rand|=(uint64_t)p[10+j]<<(8*j);
            assert(v.sec.rand_num==rand && !memcmp(v.sec.ltk,p+18,16) && !memcmp(v.sec.irk,p+34,16) && !memcmp(v.sec.csrk,p+50,16));
            assert(v.sec.ltk_present && v.sec.irk_present && v.sec.csrk_present && v.sec.authenticated==!peer && v.sec.sc==!peer);
            uint32_t gen; assert(!ble_bonds_db_version(p[0],p+1,&gen) && gen==ble_get32(p+68));
        }
        for(unsigned c=0;c<2;c++) {
            const uint8_t *p=old+80+(peer*4u+2+c)*128u+12;
            union ble_store_key k={0}; union ble_store_value v;
            k.cccd.peer_addr.type=p[0]; memcpy(k.cccd.peer_addr.val,p+1,6); k.cccd.chr_val_handle=ble_get16(p+8);
            assert(!ble_bonds_read(3,&k,&v) && v.cccd.flags==ble_get16(p+10) && v.cccd.value_changed==(p[12]&1u));
        }
    }
    ble_bond_peer_diag_t p; assert(!ble_bonds_peer(0,&p) && p.addr[0]==2 && p.rank==1);
    assert(!ble_bonds_peer(1,&p) && p.addr[0]==1 && p.rank==2);
}
static void migration_test(void)
{
    uint8_t old[BLE_LEGACY_OBJECT_SIZE],seed[64],raw[BLE_OBJECT_SIZE]; uint32_t ticks;
    fresh(); r4_fixture(old); assert(!ble_port_object_save(old,sizeof old));
    unsigned e=erases; reopen(); ble_bonds_flush(); assert(erases==e); fixture_check(old);
    ble_object_seed_load(seed); assert(!memcmp(seed,old+16,64));
    /* The ordinary boot seed advance is a next write that happens anyway. */
    boot_seed(); assert(erases==e+1); reopen(); fixture_check(old);
    assert(ble_port_object_load(raw,sizeof raw)==sizeof raw && raw[4]==2);
    /* Every possible byte cut of that migration retains all bonds and seed
     * as a whole old or new snapshot. No boot-only migration erase. */
    fresh(); assert(!ble_port_object_save(old,sizeof old)); memcpy(baseline,nor+0xde000u,8192);
    memcpy(seed,old+16,64); seed[12]^=0x80; ble_put32(seed+60,ble_crc32(seed,60));
    for(unsigned n=0;n<=4096+BLE_OBJECT_SIZE+32;n++) {
        memcpy(nor+0xde000u,baseline,8192); cut=-1; reopen(); cut=(int)n;
        (void)ble_object_seed_save(seed,&ticks); cut=-1; reopen(); fixture_check(old);
        uint8_t loaded[64]; ble_object_seed_load(loaded);
        assert(!memcmp(loaded,old+16,64) || !memcmp(loaded,seed,64));
    }
    printf("R4 fixture: both peers/directions/all keys/properties/CCCDs/generations/seed, load ZERO writes, normal seed write migrates; %u migration byte cuts: PASS\n",4096+BLE_OBJECT_SIZE+33);
}
static void full_snapshot_cuts(void)
{
    uint8_t previous[BLE_OBJECT_SIZE],next[BLE_OBJECT_SIZE],got[BLE_OBJECT_SIZE];
    fresh(); boot_seed();
    for(unsigned id=1;id<=8;id++) {
        union ble_store_value v=value(id,0x30+id);
        assert(!ble_bonds_write(1,&v)); v.sec.irk_present=1; v.sec.irk[0]=(uint8_t)id; assert(!ble_bonds_write(2,&v));
        memset(&v,0,sizeof v); v.cccd.peer_addr.val[0]=(uint8_t)id; v.cccd.peer_addr.val[5]=0x21;
        v.cccd.chr_val_handle=8; v.cccd.flags=2; assert(!ble_bonds_write(3,&v));
        v.cccd.chr_val_handle=14; v.cccd.flags=1; assert(!ble_bonds_write(3,&v));
    }
    ble_bonds_flush(); assert(ble_port_object_load(previous,sizeof previous)==sizeof previous);
    memcpy(next,previous,sizeof next); next[80+1]=9; next[80+8]=1; /* replace oldest identity 1 by 9 */
    for(unsigned i=1;i<8;i++) next[80+i*144u+8]++;
    ble_put32(next+8,1); ble_put32(next+sizeof next-4,ble_crc32(next,sizeof next-4));
    memcpy(baseline,nor+0xde000u,8192);
    for(unsigned n=0;n<=4096+BLE_OBJECT_SIZE+32;n++) {
        memcpy(nor+0xde000u,baseline,8192); cut=(int)n;
        (void)ble_port_object_save(next,sizeof next); cut=-1;
        assert(ble_port_object_load(got,sizeof got)==sizeof got);
        assert(!memcmp(got,previous,sizeof got) || !memcmp(got,next,sizeof got));
        reopen(); ble_bond_diag_t d; ble_bonds_get_diag(&d); assert(d.count==8 && d.records==32 && !d.pending);
    }
    /* Corruption and short payloads with a valid platform commit header still
     * fail the format/CRC checks. Include every byte/length, not just headers. */
    for(unsigned n=0;n<BLE_OBJECT_SIZE;n++) {
        memcpy(got,next,sizeof got); got[n]^=0x80; assert(!ble_port_object_save(got,sizeof got)); reopen();
        ble_bond_diag_t d; ble_bonds_get_diag(&d); assert(!d.count);
        assert(!ble_port_object_save(next,n)); reopen(); ble_bonds_get_diag(&d); assert(!d.count);
    }
    printf("full 8-peer snapshot: %u replacement byte cuts (all 32 records, rank, counter atomic), %u corruptions and %u truncations rejected: PASS\n",4096+BLE_OBJECT_SIZE+33,BLE_OBJECT_SIZE,BLE_OBJECT_SIZE);
}
