/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Bitbazooka LLC (Les, les.audio) */
/* Panel messages (fm1.toml panel_messages = true) on fm1-base's own pieces, as the firmware builds them: felucca-bsp's usb.c
 * and ota.c, the screen reader's announce.c, panel.c and device/panelmsg_base.c (over panelmsg.h, panelmsg.c), on a model of the
 * USB registers (tests/panelmsg/fm1_usb.h, the module's own, MIT). A "computer" sends USB-MIDI into EP1 OUT through usb.c's
 * ep1_rx and reads what ep1_tx packs into EP1 IN. The main pass is main_fm1.c's: the frame chain (ann_frame,
 * panelmsg_frame), ota_service, panelmsg_poll, the update's request (panelmsg_stop). The scan is a model of fm1_input_tick's effect on
 * fm1_in (the debounced buttons and keys at the next tick, the encoders' steps at a frame's end, 11 ticks), which
 * panelmsg_tick reads around; the main loop takes the steps as plat_enc does.
 *   sh tests/panelmsg_test.sh   (it copies usb.c and ota.c so that their fm1_usb.h is the model) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

/* ---- what unity.c gives usb.c, ota.c, announce.c, panel.c and panelmsg_base.c ---- */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define FELUCCA_OTA 1
#define FELUCCA_CDC 0
#define FELUCCA_UAC 0
#define FELUCCA_ID "FM-1_1100"
#define SXQ 128u                                /* unity.c's */
#define APP_NAME "TEMPLATE"
#define APP_VERSION "DEV"
static volatile uint32_t fm1_ms;
static uint32_t in_les, panelmsg_waited;
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void host_tick(void);
static void ota_idle(void)                      /* ota_wire_send WAITING for room: never inside the module */
{
    if (in_les) { panelmsg_waited++; fprintf(stderr, "FATAL the module made ota_wire_send wait\n"); exit(1); }
    host_tick();
}
static int ota_erase(uint32_t off) { (void)off; fprintf(stderr, "FATAL flash erase\n"); exit(1); }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; fprintf(stderr, "FATAL flash program\n"); exit(1); }
static int ota_fread(uint32_t off, void *p, uint32_t n) { (void)off; memset(p, 0xFF, n); return 0; }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; fprintf(stderr, "FATAL commit\n"); exit(1); }
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
static uint32_t locks;
static inline uint32_t fm1__lock(void) { locks++; return 0; }
static inline void fm1__unlock(uint32_t v) { (void)v; locks--; }

/* the scan (fm1_input.h's fm1_in, the part panelmsg_base.c reads) and the hands on the panel */
#define FM1_NENC 7u
static volatile struct { uint32_t buttons, notes; int16_t enc_steps[FM1_NENC]; } fm1_in;
static uint32_t hw_b, hw_k, ticks;
static int32_t hw_e[FM1_NENC];
static void fm1_input_tick(void)
{
    uint32_t e;
    ticks++;
    fm1_in.buttons = hw_b;                      /* debounced: at the next tick */
    fm1_in.notes = hw_k;
    if (ticks % 11u == 0u)                      /* a frame's end: the encoders */
        for (e = 0; e < FM1_NENC; e++) {
            fm1_in.enc_steps[e] = (int16_t)(fm1_in.enc_steps[e] + hw_e[e]);
            hw_e[e] = 0;
        }
}
/* panel.c's surroundings (panel_setup is not run here) */
enum { OBJ_PANEL };
#define SYS_S 0
#define SYS_BIG 0
#define C_BLACK 0
#define C_WHITE 0
#define C_GRAY 0
static void lcd_fill(int x, int y, int w, int h, int c) { (void)x, (void)y, (void)w, (void)h, (void)c; }
static void sys_box(int x, int y, int w, int f, const char *s, int c, int m) { (void)x, (void)y, (void)w, (void)f, (void)s, (void)c, (void)m; }
static void fm1_wdt_feed(void) {}
static uint32_t fm1_input_edges(uint32_t *r) { (void)r; return 0; }
static int32_t fm1_enc_take(uint32_t e) { (void)e; return 0; }
int plat_store_load(uint32_t obj, void *dst, uint32_t max) { (void)obj; (void)dst; (void)max; return -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { (void)obj; (void)src; (void)len; return 0; }

#include "usb.c"
#include "ota.c"
#define ANN_NAME APP_NAME " " APP_VERSION
#include "announce.c"
static uint8_t bu_on;                           /* a backup streaming (backup.c's bu.on) */
static uint8_t backup_streaming(void) { return bu_on; }
static int ann_wire_send(const uint8_t *p, uint32_t n)   /* unity.c's, as it is */
{
    if (!usb.config || usb.suspended || backup_streaming() || sx_busy || SXQ - (so_w - so_r) < (n + 2u) / 3u)
        return -1;
    return ota_wire_send(p, n);
}
#ifdef __APPLE__
#define section(s) used                         /* (panel.c's .noinit: Mach-O names its sections otherwise) */
#endif
#include "panel.c"
#undef section
#include "panelmsg_base.c"

/* ---- the computer ---- */
static int host_reads = 1;
static uint8_t sx[1 << 20]; static uint32_t sx_n;
static uint32_t notes_rx, notes_tx, note_sent_at[128], note_lat_max;
static void host_tick(void)
{
    uint32_t i;
    ep1_tx();
    if (!host_reads || !(hu_reg[1][17] & 1u))
        return;
    for (i = 0; i + 3u < hu_ep1_n; i += 4u) {
        const uint8_t *p = hu_ep1_buf + i;
        uint32_t cin = p[0] & 15u, nb = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : cin == 5u ? 1u : 0u, k;
        for (k = 0; k < nb; k++) sx[sx_n++] = p[1 + k];
        if (cin == 9u) { uint32_t lat = fm1_ms - note_sent_at[p[2] & 0x7Fu]; notes_rx++; if (lat > note_lat_max) note_lat_max = lat; }
    }
    hu_reg[1][17] &= ~1u;
}
static void host_send_sysex(const uint8_t *m, uint32_t n)
{
    static uint8_t pk[64 + 4];
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t k = n - i >= 3u ? 3u : n - i, cin = k == 3u && i + 3u < n ? 4u : k == 3u ? 7u : 4u + k;
        pk[o] = (uint8_t)cin; pk[o + 1] = m[i]; pk[o + 2] = k > 1u ? m[i + 1] : 0; pk[o + 3] = k > 2u ? m[i + 2] : 0;
        o += 4u; i += k;
        if (o == 64u || i == n) {
            memcpy(ep1rx, pk, o);
            hu_reg[1][20] = 1u; hu_reg[1][22] = o; hu_reg[1][23] = 0;
            ep1_rx();
            if (hu_reg[1][20] & 1u) { fprintf(stderr, "FATAL EP1 OUT packet not taken\n"); exit(1); }
            o = 0;
        }
    }
}
static void hello(void) { const uint8_t h[] = {0xF0, PANELMSG_SYSEX_ID, 0x50, 0x4D, 1, 1, 0xF7}; host_send_sysex(h, sizeof h); }
static void reader_hello(void) { const uint8_t h[] = {0xF0, 0x7D, 0x53, 0x52, 1, 1, 0xF7}; host_send_sysex(h, sizeof h); }
static uint32_t session_entered;
static void main_pass(void)                     /* main_fm1.c's loop, the part that matters here */
{
    const uint8_t *p;
    uint32_t n, k, e;
    if (ota_frame_get(&p, &n) && (ann_frame(p, n, fm1_ms) || panelmsg_frame(p, n)))
        ota_frame_done();
    ota_service();
    in_les = 1; panelmsg_poll(); in_les = 0;
    if (usb.ota_req) { usb.ota_req = 0; panelmsg_stop(); session_entered++; }
    ann_tick(fm1_ms);
    k = fm1__lock();                            /* the app's plat_enc: the steps taken */
    for (e = 0; e < FM1_NENC; e++)
        fm1_in.enc_steps[e] = 0;
    fm1__unlock(k);
}
static void run_ms(uint32_t ms)                 /* 1 ms: ten ticks (TIMER5, 10 kHz), two USB polls, main passes */
{
    uint32_t i, t;
    for (i = 0; i < ms; i++) {
        fm1_ms++;
        for (t = 0; t < 10u; t++) {
            panelmsg_tick();
            if (t % 5u == 0u) host_tick();
            if (t % 3u == 0u) main_pass();
        }
    }
}
static void note(uint32_t n, uint32_t on)
{
    midi_out_event((on ? 0x09u | 0x90u << 8 : 0x08u | 0x80u << 8) | n << 16 | (on ? 100u : 0u) << 24);
    if (on) { note_sent_at[n] = fm1_ms; notes_tx++; }
}

/* ---- what the computer received ---- */
typedef struct { int cmd, seq, lost, n; int kind[16], id[16], val[16]; uint32_t b, k; char s[3][24]; } fr_t;
static uint32_t parse(uint32_t from, fr_t *f, uint32_t max, uint32_t *other, uint32_t *says, uint32_t *say_len)
{
    uint32_t nf = 0, i = from;
    *other = *says = 0;
    while (i < sx_n) {
        uint32_t j = i, q, m, s;
        const uint8_t *p = sx + i;
        while (j < sx_n && sx[j] != 0xF7) j++;
        if (p[0] == 0xF0 && !memcmp(p + 1, panelmsg_id, PANELMSG_IDN) && p[PANELMSG_IDN + 1] == 0x50 && p[PANELMSG_IDN + 2] == 0x4D && nf < max) {
            fr_t *x = &f[nf++];
            p += PANELMSG_IDN - 1u;                  /* (the offsets below are a one-byte ID's) */
            memset(x, 0, sizeof *x); x->cmd = p[4];
            if (x->cmd == 18)
                for (q = 7, s = 0; s < 3; s++, q++) for (m = 0; p[q] && m < 23; ) x->s[s][m++] = (char)p[q++];
            else {
                x->seq = p[5] | p[6] << 7;
                if (x->cmd == 17) { x->lost = p[9] | p[10] << 7; x->b = p[11] | p[12] << 7; x->k = p[13] | p[14] << 7 | p[15] << 14 | (uint32_t)p[16] << 21; }
                if (x->cmd == 16) for (q = 9; q + 3 + (PANELMSG_IDN - 1u) <= j - i && x->n < 16; q += 3) { x->kind[x->n] = p[q]; x->id[x->n] = p[q + 1]; x->val[x->n] = p[q + 2]; x->n++; }
            }
        } else if (p[0] == 0xF0 && p[1] == 0x7D && p[2] == 0x53 && p[3] == 0x52 && p[4] == 16) {
            (*says)++; *say_len = j - i + 1;
        } else {
            (*other)++;
        }
        i = j + 1;
    }
    return nf;
}
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); } else printf("ok   "); printf(__VA_ARGS__); printf("\n"); } while (0)
static fr_t F[8192];
/* the events in order, from frame `from` on */
static uint32_t events(uint32_t nf, int *kind, int *id, int *val, uint32_t max, uint32_t *lost, uint32_t *seq_ok)
{
    uint32_t i, j, n = 0;
    *lost = 0; *seq_ok = 1;
    for (i = 0; i < nf; i++) {
        if (i && F[i].cmd != 18 && F[i - 1].cmd != 18 && F[i].seq != F[i - 1].seq + 1) *seq_ok = 0;
        if (F[i].cmd == 17) *lost += (uint32_t)F[i].lost;
        if (F[i].cmd == 16) for (j = 0; j < (uint32_t)F[i].n && n < max; j++, n++) { kind[n] = F[i].kind[j]; id[n] = F[i].id[j]; val[n] = F[i].val[j]; }
    }
    return n;
}

int main(void)
{
    static int kind[4096], id[4096], val[4096], wk[4096], wi[4096], wv[4096];
    uint32_t nf, other, says, say_len = 0, i, from, n, lost, seq_ok, w;
    usb.up = 1; usb.config = 1;
    panel = PANEL_DEFAULT;                      /* (panel_init: no taught panel kept) */

    /* A. nobody listening: silent; panelmsg_tick records nothing; notes flow */
    for (i = 0; i < 20; i++) { hw_b ^= 1u << (i % 14u); hw_e[2] += 1; note(60 + i % 12, 1); run_ms(2); note(60 + i % 12, 0); run_ms(2); }
    hw_b = 0; run_ms(5);
    CHECK(sx_n == 0, "A. nobody listening: no SysEx on the wire (%u bytes)", sx_n);
    CHECK(panelmsg_ev_w == 0 && panelmsg_lost == 0, "A. nobody listening: the scan side recorded nothing (%u)", panelmsg_ev_w);
    CHECK(notes_rx == notes_tx, "A. every note arrives (%u of %u)", notes_rx, notes_tx);

    /* B. HELLO through usb.c; the answer; HELD; then 300 presses/releases and 300 turns with notes, in 3 s */
    hw_b = 1u << 8; run_ms(2);                  /* HOME (matrix 8) held before the computer listens */
    hello(); run_ms(30);
    nf = parse(0, F, 8192, &other, &says, &say_len);
    CHECK(nf >= 2 && F[0].cmd == 18 && !strcmp(F[0].s[0], "TEMPLATE") && !strcmp(F[0].s[1], "DEV") && !strcmp(F[0].s[2], "fm1-base"),
          "B. the HELLO answer: \"%s\" \"%s\" \"%s\"", F[0].s[0], F[0].s[1], F[0].s[2]);
    CHECK(nf >= 2 && F[1].cmd == 17 && F[1].seq == 0 && F[1].b == 1u << 8 && F[1].k == 0, "B. HELD first (seq 0): HOME held (%#x)", F[1].b);
    from = sx_n; notes_rx = notes_tx = 0; note_lat_max = 0; w = 0;
    for (i = 0; i < 600; i++) {
        if (i % 2 == 0) {                       /* a button or a key changes */
            uint32_t m = i / 2 % 20;
            if (m < 14) { hw_b ^= 1u << m; wk[w] = (hw_b >> m & 1u) ? 1 : 2; wi[w] = (int)m; }
            else { hw_k ^= 1u << (m - 14); wk[w] = (hw_k >> (m - 14) & 1u) ? 1 : 2; wi[w] = (int)(14 + m - 14); }
            wv[w++] = 0;
        } else {                                /* an encoder detent */
            uint32_t e = i / 2 % 7; int d = (i & 4) ? 1 : -1;
            hw_e[e] += d; wk[w] = 3; wi[w] = 48 + (int)e; wv[w++] = 64 + d;
        }
        if (i % 4 == 0) note(48 + i / 4 % 24, 1);
        if (i % 4 == 2) note(48 + (i - 2) / 4 % 24, 0);
        if (i % 400 == 399) hello();
        run_ms(5);
    }
    run_ms(50);
    nf = parse(from, F, 8192, &other, &says, &say_len);
    n = events(nf, kind, id, val, 4096, &lost, &seq_ok);
    { uint32_t same = n == w;
      for (i = 0; same && i < w; i++) same = kind[i] == wk[i] && id[i] == wi[i] && val[i] == wv[i];
      CHECK(same && lost == 0 && seq_ok && other == 0, "B. %u of %u moves arrive in the order played, the protocol's ids (none lost %u, seq %u, foreign %u)", n, w, lost, seq_ok, other); }
    CHECK(notes_rx == notes_tx, "B. every note arrives while an app listens (%u of %u), longest wait %u ms", notes_rx, notes_tx, note_lat_max);
    CHECK(panelmsg_waited == 0, "B. the module never made ota_wire_send wait");

    /* C. the computer stops reading for 2 s: nothing waits, at most one panel-messages frame in the ring, then HELD says what was lost */
    from = sx_n; host_reads = 0;
    { uint32_t min_free = SXQ, presses = 0;
      for (i = 0; i < 2000; i++) { if (i % 5 == 0) { hw_b ^= 1u << (i / 5 % 14); presses++; } if (i % 500 == 0) hello(); run_ms(1);
          if (SXQ - (so_w - so_r) < min_free) min_free = SXQ - (so_w - so_r); }
      CHECK(min_free >= SXQ - 22u, "C. computer stalled: at most one panel-messages frame waited, %u of %u packets stayed free", min_free, SXQ);
      host_reads = 1; hello(); run_ms(300);
      nf = parse(from, F, 8192, &other, &says, &say_len);
      n = events(nf, kind, id, val, 4096, &lost, &seq_ok);
      CHECK(lost > 0 && n + lost == presses, "C. after the stall: %u arrived + %u reported lost = %u", n, lost, presses); }

    /* D. the screen reader beside the panel messages: its HELLO answered, every SAY whole (ANN_MAX), the panel messages streaming all along */
    from = sx_n;
    { char t[ANN_MAX + 1]; uint32_t said = 0;
      memset(t, 'A', ANN_MAX); t[ANN_MAX] = 0;
      reader_hello(); run_ms(10);
      for (i = 0; i < 400; i++) { hw_b ^= 1u << (i % 14); hw_e[i % 7] += 1;
          if (i % 20 == 0) { ann_say(t, 0, 0); said++; } if (i % 150 == 0) { hello(); reader_hello(); } run_ms(3); }
      run_ms(50);
      nf = parse(from, F, 8192, &other, &says, &say_len);
      n = events(nf, kind, id, val, 4096, &lost, &seq_ok);
      CHECK(says == said && say_len == ANN_MAX + 7u && lost == 0 && n == 800, "D. the reader's %u SAYs of %u (each %u bytes) all whole, the panel messages' %u moves too (lost %u)", says, said, say_len, n, lost); }

    /* E. a backup streaming owns the ring: the module sends nothing until it ends, then what was played */
    from = sx_n; bu_on = 1;
    for (i = 0; i < 20; i++) { hw_b ^= 1u << 5; run_ms(5); }
    nf = parse(from, F, 8192, &other, &says, &say_len);
    CHECK(nf == 0, "E. backup streaming: no panel-messages frame (%u)", nf);
    bu_on = 0; hello(); run_ms(30);
    nf = parse(from, F, 8192, &other, &says, &say_len);
    n = events(nf, kind, id, val, 4096, &lost, &seq_ok);
    CHECK(n == 20 && lost == 0, "E. after it: the 20 presses and releases (%u)", n);

    /* F. a panel taught by HARDWARE CALIBRATION (FX and SEL wired the other way round, KNOB 1 and KNOB 2 swapped,
          KNOB 1 reversed): the module still sends the printed labels' ids, clockwise + */
    hw_b = 0; run_ms(10);
    panel.btn[B_FX] = 3; panel.btn[B_SEL] = 2;
    panel.enc[EN_K1] = 3; panel.enc[EN_K2] = 2; panel.dir[EN_K1] = -1;
    from = sx_n;
    hw_b = 1u << 3; run_ms(5);                  /* matrix 3: FX on this panel */
    hw_e[3] += 1; run_ms(5);                    /* matrix encoder 3, its + is KNOB 1 turned left */
    hw_e[2] += 1; run_ms(30);                   /* matrix encoder 2: KNOB 2 right (then the frame's 8 ms and more) */
    nf = parse(from, F, 8192, &other, &says, &say_len);
    n = events(nf, kind, id, val, 4096, &lost, &seq_ok);
    CHECK(n == 3 && kind[0] == 1 && id[0] == 2 && kind[1] == 3 && id[1] == 50 && val[1] == 63 && id[2] == 51 && val[2] == 65,
          "F. taught panel: FX press id %d, KNOB 1 id %d value %d, KNOB 2 id %d value %d", id[0], id[1], val[1], id[2], val[2]);
    { const uint8_t bye[] = {0xF0, PANELMSG_SYSEX_ID, 0x50, 0x4D, 2, 0xF7}; host_send_sysex(bye, sizeof bye); run_ms(5); }
    hello(); run_ms(30);
    nf = parse(from, F, 8192, &other, &says, &say_len);
    CHECK(F[nf - 1].cmd == 17 && F[nf - 1].b == 1u << 2, "F. taught panel: HELD says FX (bit 2: %#x)", F[nf - 1].b);
    panel = PANEL_DEFAULT; hw_b = 0; run_ms(10);

    /* G. the updater's handshake while the module streams: answered once, intact (to the plain query, ota.c's
          identity cut to its name, '_' and three digits, then zeros: "FM-1_110") */
    from = sx_n;
    { uint8_t msg[7] = {0, 0x59, 0x11, 0, 0, 0, 0xFF}, wv7[16]; uint32_t k = 0, got = 0; wv7[k++] = 0xF0; k += ota_pack7(msg, 7, wv7 + k); wv7[k++] = 0xF7;
      for (i = 0; i < 200; i++) { hw_b ^= 1u << (i % 14); if (i == 100) host_send_sysex(wv7, k); run_ms(1); }
      run_ms(20);
      for (i = from; i < sx_n; ) { uint32_t j = i; while (j < sx_n && sx[j] != 0xF7) j++;
          if (sx[i + 1] != 0x7D && memcmp(sx + i + 1, panelmsg_id, PANELMSG_IDN)) { uint8_t dec[64]; uint32_t d = ota_unpack7(sx + i + 1, j - i - 1, dec, sizeof dec);
              if (d == 34 && dec[0] == 0 && dec[1] == 0x59 && dec[2] == 0x11 && !memcmp(dec + 6, "FM-1_110\0\0", 10)) got++; }
          i = j + 1; }
      CHECK(got == 1, "G. the updater's handshake answered once, intact, with the module streaming (%u)", got); }

    /* H. "upgrade": the session is entered and the module is quiet from that pass on */
    { const uint8_t up[] = {0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7}; uint32_t s0 = session_entered, q0, w0;
      run_ms(20); q0 = panelmsg_seq;
      host_send_sysex(up, sizeof up);
      run_ms(1); w0 = so_w;
      for (i = 0; i < 100; i++) { hw_b ^= 1u << (i % 14); run_ms(1); }
      CHECK(session_entered == s0 + 1 && !panelmsg_on && panelmsg_seq <= q0 + 1u && so_w == w0, "H. upgrade asked: session entered, listening off, nothing queued after it (%u packets)", so_w - w0); }


    /* I. the echo: one HELLO, then every frame of this protocol the FM-1 sends is fed back into its USB input (a MIDI
          route that loops the FM-1 to itself). Its IDENTITY, HELD and EVENTS go through the frame chain and are
          ignored: one IDENTITY, then it stops 5 s after the only HELLO and stays silent */
    { uint32_t scan, ids = 0, back = 0, ms, t0, stopped = 0, at_stop = 0;
      run_ms(50); scan = sx_n; hello(); t0 = fm1_ms;
      for (ms = 0; ms < 20000; ms++) {
          if (ms % 9 == 0) hw_b ^= 1u << (ms % 14);
          run_ms(1);
          while (scan < sx_n) { uint32_t j = scan; while (j < sx_n && sx[j] != 0xF7) j++;
              if (j < sx_n && !memcmp(sx + scan + 1, panelmsg_id, PANELMSG_IDN)) { if (sx[scan + PANELMSG_IDN + 3] == 18) ids++;
                  if (sx_made == sx_taken) { host_send_sysex(sx + scan, j - scan + 1); back++; } }
              scan = j + 1; }
          if (!panelmsg_on && !stopped) { stopped = fm1_ms; at_stop = sx_n; }
      }
      { uint32_t k, after = 0; for (k = at_stop; k < sx_n; k++) if (k + PANELMSG_IDN < sx_n && sx[k] == 0xF0 && !memcmp(sx + k + 1, panelmsg_id, PANELMSG_IDN)) after++;
        CHECK(ids == 1 && stopped && stopped - t0 <= 5003u && back > 10 && after == 0,
              "I. echoed through usb.c and the frame chain: %u IDENTITY, %u of its own frames fed back and ignored, stopped %u ms after the HELLO, %u frames after", ids, back, stopped - t0, after); } }

    CHECK(hu_bad == 0 && locks == 0, "the register model saw no misuse (%u), locks balanced (%d)", hu_bad, (int)locks);
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
