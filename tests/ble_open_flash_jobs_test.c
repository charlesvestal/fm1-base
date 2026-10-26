/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (c) 2026 Julius Vitkauskas */
/* PR1C: an in-app update, a restore and a backup while open Bluetooth advertises or has a link.
 *
 * The actual pinned NimBLE host, HCI/LL/SMP/CCM and the deterministic test central of ble_s3_integration.c, and
 * fm1-base's own flash-job code: bsp/chip/ota.c (the update's staging), device/backup.c, storage.c and ble_open.c
 * whole, and the flash glue of unity.c, plat_fm1.c and ble_open_platform.c cut verbatim from the device sources
 * (run_ble_s3_tests.py --flash-jobs, which also checks main_fm1.c's two call sites that update() and restore()
 * below repeat). The NOR model has the open build's RAM-driver gate (FL_CAN_START, open_ble/hal/fm1_ble_flash.h):
 * no erase or page program unless the controller neither advertises nor has a link; every write that lands is
 * checked against the radio's state again. No hardware, network or device I/O. */
#define main ble_s3_integration_main
#include "ble_s3_integration.c"
#undef main
#include "../include/plat.h"
#include "../open_ble/ble_health.h"
#include "flash_ranges.inc"

/* ------------------------------------------------------------------ the world: the radio, the central --- */
static int peer_deaf;                        /* the central no longer answers (gone, out of range) */
static uint32_t peer_next;
static void world_step(void)                 /* one millisecond: the LL and controller workers, the central's events */
{
    sim_advance_us(1000);
    if(ble_link.state!=BLE_LINK_IDLE && !peer_deaf && (int32_t)(sim_ticks-peer_next)>=0) {
        peer_next=sim_ticks+24u*30000u;          /* its 30 ms connection interval: an empty PDU, then it takes ours */
        if(ble_link.state==BLE_LINK_UP) (void)ble_ll_rx(1,1,0,sim_ticks,0);
        pump();
    }
    ble_port_refill();
}
static int radio_idle(void)                  /* the device's bb.state 0 and no link */
{
    ble_diag_t c; ble_get_diag(&c);
    return !c.enabled && !c.stopping && ble_link.state==BLE_LINK_IDLE;
}
static int advertising(void) { ble_diag_t c; ble_get_diag(&c); return c.enabled && !c.stopping; }
int ble_port_storage_idle(void) { return sim_flash_idle && radio_idle(); }   /* ble_open_platform.c's */

/* ------------------------------------------------------- the NOR and felucca-bsp's RAM driver, open gate --- */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BLE, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u}
static uint8_t nor[0x100000],snap[0x100000];
unsigned writes,erases;                      /* (ble_bond_capacity_test.inc's) */
static unsigned refused,landed_busy;
static uint8_t flash_ok=1,ble_store_valid=1,ble_started;
static int fm1_ble_flash_allowed(void) { return ble_port_storage_idle(); }
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static void audio_silence(void) {}
static void landed(void) { if(!radio_idle()) landed_busy++; }
static int fl_erase4k(uint32_t off,uint32_t *took)
{
    *took=0;
    if(!fm1_ble_flash_allowed()) { refused++; return -8; }      /* FL_CAN_START, inside the window */
    if((off&0xFFFu) || !FL_RANGE_OK(off,0x1000u)) return -1;
    landed(); memset(nor+off,255,4096); erases++; return 0;
}
static int fl_write(uint32_t off,const void *src,uint32_t n)
{
    const uint8_t *s=src;
    while(n) {                                                   /* once per page, as fl_write */
        uint32_t chunk=256u-(off&255u);
        if(chunk>n) chunk=n;
        if(!fm1_ble_flash_allowed()) { refused++; return -8; }
        if(!FL_RANGE_OK(off,chunk)) return -1;
        landed();
        for(uint32_t i=0;i<chunk;i++) nor[off+i]&=s[i];
        writes++; off+=chunk; s+=chunk; n-=chunk;
    }
    return 0;
}
static int st_read(uint32_t off,void *dst,uint32_t n)           /* fl_read_plain: the plain XIP window */
{
    if(off<0x93000u || off>0x100000u || n>0x100000u-off) return -1;
    memcpy(dst,nor+off,n); return 0;
}
#include "flash_store_seam.inc"                                  /* unity.c: st_erase, st_prog */
#include "../device/storage.c"

/* ------------------------------------------------------------------------ the update (bsp ota.c) --- */
#define FELUCCA_ID "FM-1_1100"
#include "../bsp/chip/ota.c"
#include "flash_ota_seam.inc"                                    /* unity.c: ota_in_area, ota_erase, ota_prog, ota_fread */

/* the host (the web installer, tools/fm1_install.py, tools/fm1_backup.py): serves its image to read requests */
static const uint8_t *host_img;
static uint32_t host_len,host_notes[16],host_note_n,host_requests,host_confirms,host_first_ms;
static uint8_t reply[720],backup_img[8192],backup_flags;
static uint32_t reply_n,backup_len,backup_end_len,backup_end_crc;
static int reply_ready,backup_end_seen;
static void host_reply(uint8_t fl,uint32_t addr,const uint8_t *data,uint32_t len)
{
    uint8_t m[15+512]; uint32_t i,s=0;
    assert(len<=512);
    m[0]=0; m[1]=0x59; m[2]=0x30; m[3]=(uint8_t)(len+8u); m[4]=(uint8_t)((len+8u)>>8); m[5]=0;
    m[6]=fl; ota_wr32(m+7,addr); m[11]=(uint8_t)len; m[12]=(uint8_t)(len>>8); m[13]=0;
    memcpy(m+14,data,len);
    for(i=6;i<14u+len;i++) s+=m[i];
    m[14+len]=(uint8_t)~s;
    reply_n=ota_pack7(m,15u+len,reply); assert(reply_n<=sizeof reply); reply_ready=1;
}
static int ota_wire_send(const uint8_t *p,uint32_t n)
{
    uint8_t d[200]; uint32_t m;
    assert(n>=9 && p[0]==0xF0 && p[n-1]==0xF7);
    m=ota_unpack7(p+1,n-2,d,sizeof d);
    assert(m>=7 && m<=sizeof d && !d[0] && d[1]==0x59);
    if(d[2]==0x30) {                                             /* a read request: fl, addr, len */
        uint32_t addr=ota_rd32(d+7),len=ota_rd16(d+11); uint8_t data[512]={0};
        if(!host_requests++) host_first_ms=ble_port_ms();
        if(addr>=0xC0000000u) {                                  /* the update's confirm, a restore's notes */
            if(addr==0xE0000000u) { memcpy(data,"success",8); host_confirms++; }
            else host_notes[host_note_n++%16u]=addr;
            host_reply(d[6],addr,data,len); return 0;
        }
        assert(host_img && len<=512 && addr+len<=host_len);
        host_reply(d[6],addr,host_img+addr,len); return 0;
    }
    if(d[2]==0x42) {                                             /* a backup's frame: offset, bytes */
        uint32_t off=ota_rd32(d+6),k=ota_rd16(d+3)-4u;
        assert(off==backup_len && off+k<=sizeof backup_img);
        memcpy(backup_img+off,d+10,k); backup_len+=k; return 0;
    }
    if(d[2]==0x43) {                                             /* its end: length, CRC-32, status, flags */
        backup_end_len=ota_rd32(d+6); backup_end_crc=ota_rd32(d+10); backup_flags=d[15]; backup_end_seen=1;
        return 0;
    }
    return 0;
}
static int ota_frame_get(const uint8_t **p,uint32_t *n) { if(!reply_ready) return 0; *p=reply; *n=reply_n; return 1; }
static void ota_frame_done(void) { reply_ready=0; }
static uint32_t ota_now_ms(void) { return ble_port_ms(); }
static void ble_poll(void);
static void radio_clean_exit(void);
static void ota_idle(void) { world_step(); ble_poll(); }        /* (more than the device: a host pass every ms) */
static uint32_t ota_steps,commits,ble_kept_at,starts_at_stage,quiet_at_start;
static int32_t ota_code;
static uint8_t ble_snap[0x2000];
static void ota_show(uint32_t step,int32_t code)
{
    ota_steps|=1u<<(step<9u ? step : 9u); ota_code=code;
    if(step==1u) {                                               /* the session's start: what Bluetooth left */
        memcpy(ble_snap,nor+0xDE000u,sizeof ble_snap); ble_kept_at=1; starts_at_stage=sim_starts;
        quiet_at_start=(uint32_t)radio_idle();
    }
}
static void ota_commit(const uint8_t *parm) { (void)parm; radio_clean_exit(); commits++; }

/* ------------------------------------------------------- the device's Bluetooth glue (ble_open.c, whole) --- */
static struct { uint32_t magic,count; } fm1_crash;
static struct { uint8_t p3_rst; } fm1_boot;
#define FM1_CRASH_MAGIC 0x43525348u
#define fm1_ms (ble_port_ms())
static uint32_t fm1_ticks(void) { return sim_ticks; }
static void fm1_wdt_feed(void) { world_step(); }                /* a flash job's wait: time passes, the radio runs */
static int ble_platform_start(void)
{
    if(ble_started) return -1;
    sim_executor_enabled=1; ble_init(pub,0); ble_host_start(); ble_started=1; return 0;
}
#include "../device/ble_open.c"
#include "flash_plat_seam.inc"                                   /* plat_fm1.c: plat_data_*, plat_store_*;
                                                                  * ble_open_platform.c: ble_port_object_* */
void sim_object_reset(void) { memset(nor,255,sizeof nor); writes=erases=0; ble_object_init(); }

/* ---------------------------------------------------------------- backup and restore (device/backup.c) --- */
#define APP_BACKUP_OBJS {OBJ_SETTINGS}
#define APP_BACKUP_MAGIC 0x31534A46u
#define APP_BACKUP_VERSION 1
static uint8_t settings[64];
static int settings_dirty,flush_rc=1;
static unsigned asked,reloads;
static void app_panic(void) {}
static int app_backup_playing(void) { return 0; }
static void app_backup_flush(void)
{
    if(settings_dirty) { flush_rc=plat_store_save(OBJ_SETTINGS,settings,sizeof settings); if(!flush_rc) settings_dirty=0; }
}
static void app_backup_hold(int on) { (void)on; }
#include "../device/backup.c"
#include "flash_backup_seam.inc"                                 /* unity.c: rs_begin, bu_flush, rs_rec_* */
static int rs_fetch(uint32_t addr,uint8_t *dst,uint32_t n) { return ota_read(addr,dst,n); }
static int rs_note(uint32_t addr) { uint8_t b[8]; ota_deadline=0; return ota_read(addr,b,8); }
static int rs_playing(void) { return 0; }
static void rs_show(uint32_t pct) { (void)pct; }
static int rs_ask(void) { asked++; assert(radio_idle()); return 1; }   /* the question: Bluetooth stays quiet */
static void rs_reload(void) { reloads++; }
static int bu_room(void) { return 1; }
static int bu_send(const uint8_t *p,uint32_t n) { return ota_wire_send(p,n); }
static void bu_hold(int on) { (void)on; }

/* ------------------------------------------------------------------- the main loop's call sites (main_fm1.c) --- */
static void loop_ms(unsigned ms) { while(ms--) { world_step(); ble_poll(); } }
static int update(void)                       /* fm1_main, usb.ota_req; a commit restarts the FM-1 (see reboot) */
{
    int rc;
    app_panic();
    flash_quiet_begin(FLASH_QUIET_UPDATE);
    rc=ota_session();
    if(!commits) flash_quiet_end();
    return rc;
}
static int restore(uint32_t len,uint32_t crc)  /* usb_cmd, RS_REQ */
{
    int begun=0,rc;
    if(bu.on) bu_stop();
    ota_try_ms=300u; ota_deadline=ota_now_ms()+3000u;
    rc=restore_rx(len,crc,&begun);
    flash_quiet_end();
    ota_try_ms=5000u; ota_deadline=0;
    return rc;
}
static void backup(void)                      /* usb_cmd, BU_REQ; bu_step from the main loop */
{
    backup_len=backup_end_len=0; backup_end_seen=0;
    bu_start(app_backup_playing());
    for(unsigned i=0;bu.on && i<10000u;i++) { bu_step(); loop_ms(1); }
    assert(!bu.on && backup_end_seen && backup_end_len==backup_len && backup_end_crc==bu_crc(0,backup_img,backup_len));
}

/* ------------------------------------------------------------------------------------ a package to install --- */
#define LOADER_LEN 6000u
static uint8_t pkg[0x400u+0x20u+LOADER_LEN];
static void make_package(void)
{
    uint8_t *h=pkg,*e=pkg+0x40,*o=pkg+0x400,*body=o+0x20; uint32_t i;
    memset(pkg,0,sizeof pkg);
    for(i=0;i<LOADER_LEN;i++) body[i]=(uint8_t)(i*7u+3u);
    ota_wr32(body+4,0x10000u); ota_wr32(body+8,0x01C0A800u);     /* the loader's inner head, Felucca's own */
    memcpy(body+0x28,"FELUCCA-LOADER-1",16);
    ota_wr16(body,ota_crc16(body+2,30,0));
    ota_wr32(o+4,0x20u); ota_wr32(o+8,LOADER_LEN); o[12]=0x41u;   /* its JLFS head */
    ota_wr16(o+2,ota_crc16(body,LOADER_LEN,0)); ota_wr16(o,ota_crc16(o+2,30,0));
    ota_wr16(e,100); ota_wr32(e+8,0x400u); ota_wr32(e+12,LOADER_LEN+0x20u); ota_jl_enc(e,0x50);   /* ota.bin */
    ota_wr16(h+8,1); ota_wr16(h+2,ota_crc16(e,0x50,0)); ota_wr16(h,ota_crc16(h+2,0x3E,0)); ota_jl_enc(h,0x40);
}
static void staged_ok(void)
{
    assert(!memcmp(nor+OTA_AREA+32u,pkg+0x420u,LOADER_LEN) && ota_rd32(nor+OTA_AREA+4)==OTA_AREA);
    assert(ota_rd16(nor+OTA_RES+2)==0x5A0Du && ota_rd16(nor+OTA_RES+6)==0x5441u &&
           ota_rd16(nor+OTA_RES)==ota_crc16(nor+OTA_RES+2,78,0));
}

/* ------------------------------------------------------------------------------------------------ helpers --- */
static void wait_advertising(void)
{
    unsigned i;
    for(i=0;i<2000u && !(advertising() && plat_ble_state()==BLE_READY);i++) loop_ms(1);
    assert(advertising() && plat_ble_state()==BLE_READY);
}
static void connect_timeout(uint16_t timeout)  /* connect_link, with the central's supervision timeout */
{
    ble_conn_t c=sim_connection(); c.timeout=timeout;
    assert(advertising());                       /* a central connects only to an advertising FM-1 */
    tx_encrypted=started=start_req_seen=0; tx_counter=rx_counter=0; smp_n=l2_n=l2_want=0; sim_slot[0]=sim_slot[1]=-1;
    assert(ble_link.state==BLE_LINK_IDLE && ble_ll_create(&c,sim_ticks)); assert(ble_ll_rx(1,1,0,sim_ticks,0)); pump();
    control_rx((uint8_t[9]){8,1,0,0,0,0,0,0,0},9);
    control_rx((uint8_t[6]){12,6,1,0,2,0},6);
    observe_connection(); peer_next=sim_ticks;
    loop_ms(5); assert(plat_ble_linked());
}
static uint8_t saved_ltk[16],rand_ediv[10];
static void reconnect(void)
{
    ble_host_diag_t d;
    wait_advertising(); connect_timeout(500); encryption_start(saved_ltk,rand_ediv); loop_ms(40);
    ble_host_get_diag(&d); assert(d.encrypted && d.bonded && d.key_size==16 && !d.pair_failures);
}
static void bond_present(void)                /* from the NOR, as the next start reads it */
{
    union ble_store_key k; union ble_store_value v;
    ble_object_init(); ble_bonds_init();
    memset(&k,0,sizeof k); memcpy(k.sec.peer_addr.val,central,6);
    assert(!ble_bonds_read(1,&k,&v) && v.sec.ltk_present && !memcmp(v.sec.ltk,saved_ltk,16));
}
static void settings_are(char c)
{
    uint8_t got[64]; assert(plat_store_load(OBJ_SETTINGS,got,sizeof got)==(int)sizeof got);
    for(unsigned i=0;i<sizeof got;i++) assert(got[i]==(uint8_t)c);
}
static void subscribe_midi(uint16_t midi_start)
{
    uint8_t q[]={18,0,0,1,0}; ble_put16(q+1,(uint16_t)(midi_start+3u)); l2send(4,q,sizeof q); assert(att[0]==19);
    loop_ms(5); assert(plat_ble_connected());
}

int main(void)
{
    ble_host_diag_t d; ble_bond_diag_t bond;
    uint32_t t0,starts,terms,timeouts,backup_b_len,backup_b_crc;
    (void)timeouts;
    static uint8_t backup_b[8192];
    uint16_t midi_start;
    memcpy(peer_irk,sample_irk,16); peer_terminate_ok=1;
    sim_reset(); sim_storage_reset(); sim_seed(); make_package();
    memset(settings,'A',sizeof settings);
    assert(!plat_store_save(OBJ_SETTINGS,settings,sizeof settings));          /* Bluetooth not started: idle */
    plat_ble_start(); wait_advertising();

    /* 1. PR1/PR1B's behaviour, reached now only by skipping the hold: while advertising, the update's and the
     *    restore's writes are refused (-8) and the update fails (FAILED), nothing written; an object save pauses
     *    advertising itself and is allowed. */
    memcpy(snap,nor,sizeof nor); refused=0; host_img=pkg; host_len=sizeof pkg;
    assert(ota_erase(OTA_AREA)==-8 && rs_rec_erase()==-8 && st_erase(st_sector(OBJ_SETTINGS,1))==-8);
    assert(ota_session()==-8 && ota_code==-8 && !host_confirms && !commits);
    assert(refused>=3 && !memcmp(nor,snap,sizeof nor) && advertising());
    memset(settings,'A',sizeof settings); assert(!plat_store_save(OBJ_SETTINGS,settings,sizeof settings));
    wait_advertising();
    puts("flash jobs: without the hold, an update/restore write while advertising is refused (-8, FAILED, NOR unchanged); an object save pauses advertising: PASS");

    /* 2. An update while advertising: advertising stopped for the whole session (it never restarts, though the
     *    host runs between sectors), every write allowed, staged and committed. */
    refused=landed_busy=0; terms=peer_terminates; ota_steps=0;
    assert(!update() && commits==1 && host_confirms==1 && !refused && !landed_busy && peer_terminates==terms);
    assert(quiet_at_start && sim_starts==starts_at_stage && radio_idle() && plat_ble_state()==BLE_OFF);
    staged_ok();
    assert(ota_boot_cleanup()==1 && nor[OTA_AREA]==0xFFu && nor[OTA_RES]==0xFFu);   /* the next start */
    flash_quiet_end(); commits=0; host_confirms=0; wait_advertising();
    puts("flash jobs: update while advertising: quiet for the whole session, staged + committed, Bluetooth back: PASS");

    /* 3. A Mac-like central: connects, pairs (SC), subscribes to MIDI. The new bond waits for the link's end. */
    connect_timeout(500); midi_start=discover_all();
    pair_link(1,0,saved_ltk,rand_ediv); subscribe_midi(midi_start);
    ble_bonds_get_diag(&bond); assert(bond.pending);

    /* 4. Without the hold, nothing writes mid-link: update/restore staging, object and data saves are refused;
     *    the link stays up and the NOR is unchanged. */
    memcpy(snap,nor,sizeof nor); refused=0;
    assert(ota_erase(OTA_AREA)==-8 && rs_rec_erase()==-8 && st_erase(st_sector(OBJ_SETTINGS,1))==-8);
    assert(st_prog(st_sector(OBJ_SETTINGS,1)+256u,settings,16)==-8);
    assert(plat_store_save(OBJ_SETTINGS,settings,sizeof settings)==-8);
    assert(plat_data_erase(0x97000u)==-8 && plat_data_write(0x97000u,settings,16)==-8);
    assert(!memcmp(nor,snap,sizeof nor) && refused>=3 && plat_ble_connected() && peer_terminates==terms);
    puts("flash jobs: mid-link without the hold: staging, store, data writes refused (-8), link kept, NOR unchanged: PASS");

    /* 5. An update while linked: LL_TERMINATE_IND 0x15 (power off), acknowledged, a normal close (not a
     *    supervision timeout); the bond the link left saved before the session; the session's writes never touch
     *    it; at the next start the bond is read back and the central reconnects encrypted without pairing. */
    refused=landed_busy=0; terms=peer_terminates; timeouts=ble_link.diag.terminate_timeouts; ble_kept_at=0;
    t0=ble_port_ms(); host_requests=0;
    assert(!update() && commits==1 && !refused && !landed_busy);
    assert(peer_terminates==terms+1u && peer_terminate_reason==0x15 && ble_link.diag.terminate_timeouts==timeouts);
    assert(ble_link.diag.reason==0x16 && !plat_ble_linked() && ble_kept_at && quiet_at_start && sim_starts==starts_at_stage);
    assert(host_first_ms-t0<1000u);                              /* the session's first request: at once */
    ble_bonds_get_diag(&bond); assert(!bond.pending && !bond.failures);
    assert(!memcmp(ble_snap,nor+0xDE000u,sizeof ble_snap));      /* the session left Bluetooth's object alone */
    staged_ok();
    assert(ota_boot_cleanup()==1);
    bond_present();
    flash_quiet_end(); commits=0; host_confirms=0;
    reconnect(); subscribe_midi(midi_start);
    puts("flash jobs: update while linked: LL_TERMINATE_IND 0x15 acknowledged, normal close, bond saved first and intact, reconnect encrypted without pairing: PASS");

    /* 6. A backup while linked, nothing unsaved: the link is kept. */
    terms=peer_terminates; settings_dirty=0;
    backup(); assert(peer_terminates==terms && plat_ble_connected() && !backup_flags);
    /* 7. Something unsaved: the backup's save ends the link first (0x13), the image holds it, Bluetooth back. */
    memset(settings,'B',sizeof settings); settings_dirty=1; flush_rc=1; refused=landed_busy=0;
    backup();
    assert(!flush_rc && !settings_dirty && peer_terminates==terms+1u && peer_terminate_reason==0x13);
    assert(!refused && !landed_busy && backup_len==16u+8u+64u+4u && !memcmp(backup_img+24,settings,64));
    memcpy(backup_b,backup_img,backup_len); backup_b_len=backup_end_len; backup_b_crc=backup_end_crc;
    settings_are('B'); wait_advertising(); bond_present();
    puts("flash jobs: backup while linked: link kept with nothing to save; ends with 0x13 to save, image complete, Bluetooth back: PASS");

    /* 8. Saved meanwhile (advertising, not linked), then a restore while linked: 0x13, staged, asked, committed,
     *    reloaded; Bluetooth back after it; the bond untouched; the central reconnects. */
    memset(settings,'C',sizeof settings); assert(!plat_store_save(OBJ_SETTINGS,settings,sizeof settings));
    settings_are('C'); reconnect();
    terms=peer_terminates; refused=landed_busy=0; asked=reloads=0; host_note_n=0;
    host_img=backup_b; host_len=backup_b_len; starts=sim_starts;
    assert(!restore(backup_b_len,backup_b_crc));
    assert(peer_terminates==terms+1u && peer_terminate_reason==0x13 && asked==1 && reloads==1);
    assert(!refused && !landed_busy && host_note_n==2 && host_notes[0]==RS_ASK_ADDR && host_notes[1]==RS_OK_ADDR);
    settings_are('B'); bond_present();
    wait_advertising(); assert(sim_starts==starts+1u);           /* advertising once more: after the restore only */
    reconnect();
    puts("flash jobs: restore while linked: LL_TERMINATE_IND 0x13, staged/asked/committed, Bluetooth back, bond intact, reconnect: PASS");

    /* 9. A restore while advertising (the link ended first by the central), and Bluetooth switched off by the app
     *    during the restore: it stays off afterwards, as the app now wants. */
    disconnect_link(); wait_advertising();
    terms=peer_terminates; memset(settings,'D',sizeof settings); assert(!plat_store_save(OBJ_SETTINGS,settings,sizeof settings));
    assert(!restore(backup_b_len,backup_b_crc) && peer_terminates==terms); settings_are('B'); wait_advertising();
    plat_ble_enable(0); loop_ms(20); assert(plat_ble_state()==BLE_OFF && radio_idle());
    assert(!restore(backup_b_len,backup_b_crc)); loop_ms(300); assert(plat_ble_state()==BLE_OFF && radio_idle());
    plat_ble_enable(1); wait_advertising();
    puts("flash jobs: restore while advertising and with Bluetooth off: works, Bluetooth as the app wants after: PASS");

    /* 10. A central gone silent with its 5 s supervision timeout: the terminate is never acknowledged, the link
     *     ends at the timeout and the update goes ahead, its first request inside the hosts' 8 s. */
    reconnect(); peer_deaf=1; terms=peer_terminates; ble_kept_at=0;
    refused=0; t0=ble_port_ms(); host_img=pkg; host_len=sizeof pkg; host_requests=0;
    assert(!update() && commits==1 && !refused && peer_terminates==terms && ble_link.diag.reason==0x08);
    assert(ble_kept_at && quiet_at_start && starts_at_stage==sim_starts);
    assert(host_first_ms-t0>=4900u && host_first_ms-t0<7000u);  /* the hosts wait 8 s for it (web/fm1ota.js) */
    staged_ok();
    peer_deaf=0; assert(ota_boot_cleanup()==1); bond_present(); flash_quiet_end(); commits=0; wait_advertising();

    /* 11. One that never answers, with a 32 s timeout: the hold gives up at FLASH_QUIET_MS; the update is refused
     *     (FAILED) and writes nothing; Bluetooth comes back once the link has ended. */
    wait_advertising(); connect_timeout(3200); loop_ms(100); peer_deaf=1; memcpy(snap,nor,sizeof nor); refused=0;
    t0=ble_port_ms();
    assert(update()==-8 && ota_code==-8 && !commits && refused);
    assert(!memcmp(nor,snap,sizeof nor) && ble_port_ms()-t0>=FLASH_QUIET_MS);
    for(unsigned i=0;i<40000u && plat_ble_linked();i++) loop_ms(1);
    assert(!plat_ble_linked() && ble_link.diag.reason==0x08 && ble_port_ms()-t0>=31000u);   /* (32 s from its last packet) */
    peer_deaf=0;
    wait_advertising();
    puts("flash jobs: a silent central: update after its 5 s supervision timeout; with 32 s the hold gives up at 7 s, FAILED, nothing written, Bluetooth back: PASS");

    ble_host_get_diag(&d); assert(!d.assertions && !d.heap_fail && !d.event_drop && !d.acl_drop);
    printf("open BLE flash jobs (update, restore, backup with advertising / a link / a silent central): PASS; heap peak %u\n",d.heap_peak);
    return 0;
}
