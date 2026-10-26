/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* fm1-base's platform interface (from X0X's, by way of FoMni-1): everything the app needs from the hardware.
 *
 * The device implementation (plat_fm1.c) wraps Felucca's HAL and panel map; the
 * host implementation (host/plat_host.c) is a simulator fed by a script, which is
 * what lets the whole app — UI, sequencer, engines — run and be tested on a Mac.
 * Drawing goes through Felucca's lcd_fill / lcd_blit (lcd.c on the device, a
 * framebuffer on the host) and gfx.c on top. */
#pragma once
#include <stdint.h>

/* big zero-initialised buffers go in the .pool RAM region on the device */
#ifdef PLAT_HOST
#define PLAT_POOL
#else
#define PLAT_POOL __attribute__((section(".pool")))
#endif

/* panel controls, by printed label (Felucca's panel.c map turns these into matrix ids; Felucca
 * calls the second button SCL, the panel prints SEL) */
enum { B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC,
       B_OCTDN, B_OCTUP, NB };
enum { EN_SELECT, EN_ALGO, EN_PRESET, EN_K1, EN_K2, EN_K3, EN_K4, NE };

#define NKEYS 27                       /* F3 .. G5 */
/* key k by colour: the 16 white keys and the 11 black keys, left to right (initialisers for a uint8_t array) */
#define PLAT_WHITE_KEYS {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26}
#define PLAT_BLACK_KEYS {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25}

static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

uint32_t plat_ms(void);
uint32_t plat_buttons(void);           /* held: bit B_* */
uint32_t plat_keys(void);              /* held: bit k = key k (0 = F3) */
int32_t plat_enc(int role);            /* detents since the last call, + = clockwise; EN_* */
uint32_t plat_master(void);            /* MASTER pot's position, 0..4096 (the render gets it as a gain: APP_GAIN) */
int plat_battery(void);                /* the battery: 0 (empty) .. 3 (full), 4 a USB host powers the FM-1, -1 not read
                                        * yet (the keys' readings own the converter: APP_KEY_SENSE) */

/* lights: bright buttons and keys, and dim ones (a short pulse each scan frame; a bright bit wins); glow 0 the dim
 * ones off, 1 low, 2 high. Bit NB in buttons / dim_buttons is PLAY's second, green LED */
void plat_lights(uint32_t buttons, uint32_t keys, uint32_t dim_buttons, uint32_t dim_keys, int glow);
#define LED_PLAY_GREEN (1u << NB)
/* a third level, between the glow and lit (~12 % of lit; ~8 % with glow 1): these buttons and keys, unless lit, until
 * the next call (0, 0: none). Call it after plat_lights each time the lights change */
void plat_lights_mid(uint32_t buttons, uint32_t keys);
/* the same, simpler: the lit buttons and keys, the unlit buttons glowing if plat_glow(1); PLAY lights green, red
 * while plat_play_red(1) */
void plat_leds(uint32_t buttons, uint32_t keys);
void plat_glow(int on);
void plat_play_red(int red);

/* MIDI: 4-byte USB-MIDI event packets (byte 0 = cable / CIN, then status, data 1, data 2). In from USB, the TRS jack
 * (in only: the FM-1 has no MIDI out jack) and Bluetooth (FM1B_BLE); out to USB and Bluetooth. Bluetooth has two
 * links with FM1B_BLE_CENTRAL (below), the computer's and the device's: both come in on PORT_BLE, TO_BLE goes to
 * both, TO_BLE_HOST or TO_BLE_DEV to one. MIDI thru with nothing echoed back where it came from: after plat_midi_in,
 * plat_midi_send(pkt, (TO_USB | TO_BLE) & ~plat_midi_in_from()) */
enum { PORT_USB, PORT_BLE, PORT_TRS, NPORTS };
#define TO_USB (1u << PORT_USB)
#define TO_BLE_HOST (1u << PORT_BLE)   /* the computer (a phone, a tablet) connected to the FM-1 */
#define TO_BLE_DEV (1u << 3)           /* the Bluetooth MIDI device the FM-1 connected to (FM1B_BLE_CENTRAL) */
#define TO_BLE (TO_BLE_HOST | TO_BLE_DEV)
int plat_midi_in(uint32_t *pkt, int *port);   /* 1 = got one, from port PORT_* (port may be 0) */
uint32_t plat_midi_in_from(void);      /* where the last one came from, as an out: TO_USB, TO_BLE_HOST or TO_BLE_DEV
                                        * (0: the TRS jack, which has no out); called where plat_midi_in is */
void plat_midi_send(uint32_t pkt, uint32_t to);   /* to: TO_USB | TO_BLE, or one link; main loop, the render or the
                                                   * tick */
void plat_midi_out(uint32_t pkt);      /* plat_midi_send(pkt, TO_USB) */
uint32_t plat_midi_out_room(uint32_t to);   /* the free slots in the out queues of to (TO_USB, TO_BLE, a link: each
                                        * has its own): the fewest among the ones someone listens on; 0 when nobody
                                        * does. A sequencer keeps room for the note-offs it owes before it sends a
                                        * note-on */
uint32_t plat_midi_out_link(void);     /* a number that changes at each new USB connection (a host to tell its
                                        * sounds again: programs, volumes) */
/* USB MIDI host (fm1.toml usb_host = true: FM1B_USB_HOST 1; felucca-bsp docs/USB_HOST.md): the USB port plays host to
 * a USB-MIDI controller or synth, directly or behind one powered hub; the FM-1 gives the port no power (a powered hub
 * or adapter). It comes in on PORT_USB and TO_USB goes to it; no computer meanwhile (no USB audio, backup, mirror).
 * Boot is always a device. plat_usb_host (main loop) asks; connecting blocks the main loop a moment (seconds behind a
 * hub), the sound plays on. plat_usb_host_msg: the latest change, once (not a queue). plat_usb_lost: 1 once after a
 * USB session ended (a device lost, the mode changed): release what its notes hold. Without FM1B_USB_HOST all stay
 * USBH_OFF / 0. */
enum { USBH_OFF, USBH_LOOKING, USBH_CONNECTED };
enum { USBH_MSG_NONE, USBH_MSG_ON, USBH_MSG_OFF, USBH_MSG_MIDI, USBH_MSG_NOMIDI, USBH_MSG_LOW, USBH_MSG_LOST };
void plat_usb_host(int on);
int plat_usb_host_state(void);
int plat_usb_host_msg(void);
int plat_usb_lost(void);
uint32_t plat_midi_count(int port, int out);   /* messages through port PORT_* since the start (out: sent), real-time (clock...) left out */
uint32_t plat_midi_ago(int port, int out);     /* ms since the last one (0xFFFFFFFF: none yet) */

/* Bluetooth MIDI (fm1.toml ble = true: FM1B_BLE 1): JieLi's BT libraries on fm1-base's shim (ble/), started at
 * every boot unless the last Bluetooth run crashed (then by hand: plat_ble_start). The app gives it a store object,
 * OBJ_BLE (the radio's address and pairing). Without it (FM1B_BLE 0, tools/build.py --no-ble) nothing of JieLi's is
 * linked, and the state stays BLE_OFF */
#ifndef FM1B_BLE
#define FM1B_BLE 0
#endif
enum { BLE_OFF, BLE_STARTING, BLE_READY, BLE_FAILED };
int plat_ble_state(void);
int plat_ble_connected(void);          /* a central is connected and listening */
int plat_ble_linked(void);             /* a central is connected, listening or not yet (a Mac whose MIDI driver
                                        * never subscribed holds the link, and the FM-1 stops advertising) */
void plat_ble_start(void);
uint32_t plat_ble_log_total(void);     /* lines written since the last start (survives a reset) */
const char *plat_ble_log_line(int back);   /* 0 = the newest */
/* Bluetooth MIDI devices (fm1.toml ble_central = true: FM1B_BLE_CENTRAL 1): the FM-1 connects to one (a keyboard,
 * a controller), beside a computer connected to it. Its MIDI comes in as PORT_BLE too (plat_midi_in_from:
 * TO_BLE_DEV), and TO_BLE goes to both (TO_BLE_DEV to the device alone, TO_BLE_HOST to the computer).
 * Scanning finds the devices that advertise the MIDI service (up to 8); connecting stops the scan. */
#ifndef FM1B_BLE_CENTRAL
#define FM1B_BLE_CENTRAL 0
#endif
enum { BLE_DEV_OFF, BLE_DEV_SCANNING, BLE_DEV_CONNECTING, BLE_DEV_SEARCHING, BLE_DEV_READY };
void plat_ble_scan(int on);
int plat_ble_found(int i, char *name, int *rssi);   /* device i of the scan (name: 21 bytes); 0 = none */
int plat_ble_connect(int i);           /* 0 = connecting */
void plat_ble_disconnect(void);
int plat_ble_device(void);             /* BLE_DEV_*: READY is connected, MIDI both ways */
/* Bluetooth on or off, as the player asks: off ends every link, stops advertising and scanning (the radio quiet);
 * on starts it (or advertises again). Whether it starts at power-on: APP_BLE_AT_BOOT (app.h, e.g. a saved setting;
 * default 1) */
void plat_ble_enable(int on);

/* WiFi (fm1.toml wifi = true: FM1B_WIFI 1): a station on JieLi's WiFi libraries and the SDK's lwIP, on the same shim.
 * Off until the app turns it on; then it joins the networks it joined before (kept in the app's store object OBJ_WIFI,
 * six, the most recent first). The app's network code runs in a task of its own (plat_net_task) with lwIP's sockets
 * (lwip/sockets.h): a socket call waits, and the main loop must not. A build has Bluetooth or WiFi, not both (yet). */
#ifndef FM1B_WIFI
#define FM1B_WIFI 0
#endif
enum { WIFI_OFF, WIFI_STARTING, WIFI_ON, WIFI_JOINING, WIFI_ADDRESS, WIFI_CONNECTED, WIFI_FAILED };
void plat_wifi_on(int on);             /* on: the radio starts (its first second holds the sound) and joins */
int plat_wifi_state(void);             /* WIFI_*: ON is on with no network; ADDRESS, joined and asking for one */
const char *plat_wifi_why(void);       /* WIFI_FAILED: "not found", "wrong password?", "no address"... */
const char *plat_wifi_ssid(void);      /* the network joined, or being joined */
uint32_t plat_wifi_ip(void);           /* connected: the address, its first byte lowest; else 0 */
int plat_wifi_rssi(void);              /* connected: the signal */
int plat_wifi_scan(void);              /* the networks around: plat_wifi_found once it is done (0 = asked) */
int plat_wifi_found(int i, char *ssid, int *rssi, int *secured);   /* the scan's network i (ssid: 33 bytes), the
                                                                     * strongest first; 0 = none */
int plat_wifi_found_channel(int i);    /* its channel (1..14), 0 = none */
int plat_wifi_join(const char *ssid, const char *pass);   /* pass 0: the saved one's, or none; kept once joined */
int plat_wifi_saved(int i, char *ssid);   /* the networks kept, the most recent first; 0 = none */
void plat_wifi_forget(int i);
int plat_wifi_kept(const char *ssid);  /* the network is kept (its password known): plat_wifi_join(ssid, 0) joins it */
void plat_wifi_rx(uint32_t *frames, int *rssi);   /* reception: frames received in the last second (anyone's), their
                                                   * average signal */
uint32_t plat_wifi_scans(void);        /* scans done since the start: a change means plat_wifi_found has new results */
int plat_net_task(void (*fn)(void *), void *arg, const char *name);   /* a task for the app's network code (8 KiB
                                                                        * of stack); 0 = made */

/* the 1 kHz tick (app_tick, an app with APP_TICK) runs in the timer interrupt: the main loop locks it out around
 * what they share (it nests) */
uint32_t plat_lock(void);
void plat_unlock(uint32_t f);
void plat_yield(void);
void plat_log(const char *line);
/* the flash's data area, 0x97000..0xDFFFF, for an app's own big data (a table it builds, samples): its store objects
 * live there too, at the offsets it gave them (APP_STORE_MAP), so it keeps to the rest. Main loop only; an erase or a
 * write stops everything for a moment (the sound too: no flash reads meanwhile).
 * Two more windows for big data, outside the store: PLAT_XDATA0 (16 KiB, just above the app area) and PLAT_XDATA1
 * (92 KiB, above the update's staging). The update loader clears an update record it finds in the last 256 bytes of
 * a sector there (bsp/loader/ldr_core.c: "AT" at bytes 0xF06..0xF07, a nonzero CRC-16 at 0xF00 over the 78 bytes
 * from 0xF02), so in these two an app keeps bytes 0xF00..0xF07 of every sector erased (0xFF), or else makes sure
 * those bytes never read as a record: never "AT" at 0xF06 (LOOM-8's sample bank moves its data so, and refuses a
 * bank that has it) */
#define PLAT_DATA_LO 0x97000u
#define PLAT_DATA_HI 0xE0000u
#define PLAT_XDATA0_LO 0x93000u
#define PLAT_XDATA0_HI 0x97000u
#define PLAT_XDATA1_LO 0xE5000u
#define PLAT_XDATA1_HI 0xFC000u
#define PLAT_DATA_OK(off, n) (plat_in((off), (n), PLAT_DATA_LO, PLAT_DATA_HI) || \
                              plat_in((off), (n), PLAT_XDATA0_LO, PLAT_XDATA0_HI) || \
                              plat_in((off), (n), PLAT_XDATA1_LO, PLAT_XDATA1_HI))
static inline int plat_in(uint32_t off, uint32_t n, uint32_t lo, uint32_t hi)   /* [off, off + n) in [lo, hi) */
{
    return off >= lo && off <= hi && n <= hi - off;
}
const uint8_t *plat_data(uint32_t off);   /* read in place; 0 = outside the windows */
int plat_data_erase(uint32_t off);        /* the 4 KiB sector at off (aligned); 0 = done */
int plat_data_write(uint32_t off, const void *src, uint32_t n);   /* into erased flash, src in RAM; 0 = done */
void plat_sysex_send(const uint8_t *frame, uint32_t n);   /* a whole SysEx frame (F0 .. F7) out over USB */

/* the app's own commands in the update's SysEx framing (F0 pack7(00 59 cmd len24 body ~sum) F7: felucca-bsp ota.c),
 * an app with APP_USB_CMD: app_usb_cmd(d, n) gets each frame whose cmd the platform does not take (d: the decoded
 * frame, cmd d[2], the body from d + 6). Inside it the app may pull data from the host as an update does: */
int plat_host_read(uint32_t addr, uint8_t *dst, uint32_t n);   /* n bytes (512 at most) at the host's addr; 0 = ok.
                                                                 * Waits (tries: 3); main loop only */
void plat_host_patience(uint32_t try_ms, uint32_t total_ms);   /* each try's wait (default 5000) and the whole
                                                                 * transfer's (0: no limit) */

/* USB audio full (FM1B_UAC): 1, the USB audio input at full level (the app's render at gain 4096), MASTER for the
 * speaker and headphones only; 0, both at MASTER (the default) */
void plat_usb_audio_full(int on);

/* the keys' velocity and pressure (an app with APP_KEY_SENSE): on, the scan reads the keys on the analog rows and
 * hands each reading to the app (app_key_sample, app_key_probe); off by default */
void plat_sense(int on);
uint32_t plat_analog_mask(void);       /* bit k: key k is on an analog row (PA7 / PA8; the top row is probed) */

/* 16 bytes unique to this FM-1 (its flash part's factory ID; the same across updates, firmwares and a wiped store):
 * for an address that must not change (the WiFi MAC, the Bluetooth name); 0 = unknown (and in a build without a radio) */
const uint8_t *plat_chip_uid(void);

/* persistent storage (Felucca storage.c objects: A/B sectors, CRC, torn-write safe) */
int plat_store_load(uint32_t obj, void *dst, uint32_t max);   /* bytes loaded, < 0 = none / bad (an empty object is
                                                               * none: a restore writes one for an absent object) */
int plat_store_head(uint32_t obj, void *dst, uint32_t n);     /* its first n bytes (a header): n, < 0 = none / bad /
                                                               * shorter */
int plat_store_size(uint32_t obj);                           /* its length in bytes, < 0 = none / bad (an app that
                                                               * wants it whole or not at all checks it first) */
int plat_store_save(uint32_t obj, const void *src, uint32_t len);   /* 0 = ok */
#define PLAT_STORE_MAX 3840u           /* bytes per object */
/* The objects are the app's (app.h): an enum that includes OBJ_PANEL (the panel calibration, the platform's) and
 * ends with OBJ_NOBJ, and APP_STORE_MAP, each object's flash offset: two 4 KiB sectors (A/B) from there, inside the
 * data area (0x97000..0xDFFFF) or Felucca's settings sector pair at 0xFC000. An object's number is written in its
 * record, so an app keeps its numbering from one release to the next. */

/* audio load in % of the render budget, and xruns since boot (device: from the ISR) */
uint32_t plat_cpu_pct(void);
uint32_t plat_xruns(void);

/* a free-running count of PLAT_TICKS_PER_US ticks a microsecond, for timing spans shorter than plat_ms can: a part of
 * the render, say. Device: TIMER4, from the 24 MHz crystal (one register read); host: its monotonic clock, at the same
 * rate. No lock and no state, so the audio interrupt and the tick may call it too. It wraps every 2^32 ticks (179 s):
 * a span is the unsigned difference of two reads */
#define PLAT_TICKS_PER_US 24u
uint32_t plat_ticks24(void);

/* The second core (fm1.toml cpu2 = true: FM1B_CPU2; felucca-bsp's chip/fm1_cpu1.h, Melodee's): a worker for a part of
 * the render. Start it in app_init (the one time it may start: at power-on, before the audio and the timers); the
 * render hands it a job, does its own share, then waits for it. One job at a time. A failure (it misread a job, it
 * faulted, a job was not back in 8 ms, it did not answer at its start) restarts the FM-1, the reason in the crash
 * record (vec 0x101..0x105: gave_up's numbers below); two failed starts are safe mode. With cpu2_fallback = true
 * instead: it is given up (held; the render does the work itself) and barred, one core at every start until
 * power-off or a new build (FM1B_BUILD), and one core after a failed start. Host: a job runs at once, in order (an app
 * with its own simulation defines APP_HOST_CPU2 and has them all). */
int plat_cpu2_set(int on);             /* on 1, in app_init: 0 = it runs; -1 = one core (not now, barred, or it did not
                                        * answer). On 0: held (from the main loop: no job outstanding) */
int plat_cpu2_on(void);                /* it takes jobs */
int plat_cpu2_run(void (*fn)(uint32_t), uint32_t arg);   /* render: 1 = it has fn(arg); 0 = do it yourself */
int plat_cpu2_wait(void);              /* render: 0 = done; -1 = it was given up and the job dropped: do it yourself
                                        * (cpu2_fallback only) */
void plat_cpu2_sync(void);             /* a barrier between the cores' reads and writes (a job's own sharing) */
int plat_cpu2_busy(void);              /* 1 = the job handed over is not done yet (no waiting) */
int plat_cpu2_gave_up(void);           /* why it is off: 0 no, 1 it misread a job, 2 one was late, 3 barred, 4 it
                                        * faulted, 5 it did not answer at its start */
uint32_t plat_cpu2_bad(void);          /* the jobs it misread */
int plat_cpu2_barred(void);            /* 1: one core until power-off (given up, or the first core crashed beside it) */
uint32_t plat_cpu2_stack(void);        /* how deep its stack (2816 B) has gone, in bytes */

/* the app's optional hooks (app.h defines the macro when it has the function):
 *   APP_TICK        void app_tick(void)           1 kHz, in the timer interrupt (a clock, a sequencer)
 *   APP_BOOT        void app_boot(void)           once, after the platform is up, before app_ui_init (a radio)
 *   APP_LOOP        void app_loop(void)           every pass of the main loop (a radio's tasks)
 *   APP_YIELD       void app_yield(void)          from plat_yield, inside long work
 *   APP_OVERLOAD    void app_overload(void)       the render took over 90 % of its time (shed voices)
 *   APP_SPLASH      void app_splash(void)         the first screen at boot, instead of the name
 *   APP_GAIN(m)     the render's gain from MASTER's position m (0..4096; default the square, m * m >> 12)
 *   APP_QUIET       void app_quiet(int32_t *lr, uint32_t n)   the render while the flash must not be read (the
 *                   radio's first second, FM1B_BLE): keep time without sound (default: silence)
 *   APP_KEY_SENSE   void app_key_sample(uint32_t key, uint32_t v, uint32_t ticks, int driven) (rows PA7 / PA8),
 *                   int app_key_drive(uint32_t key) (read it driven), void app_key_probe(uint32_t key, uint32_t v,
 *                   uint32_t pressed) (the top row), int app_key_led_ok(uint32_t key) (0: its column dark when read);
 *                   from the scan's interrupt
 *   APP_SYSEX       int app_sysex(const uint8_t *p, uint32_t n)   a SysEx frame from USB the platform does not take
 *                   (p: the bytes between F0 and F7); 1 = the app's. Its answers: plat_sysex_send. Main loop
 *   APP_USB_CMD     void app_usb_cmd(const uint8_t *d, uint32_t n)   a frame in the update's framing (above). Main loop
 *   APP_REDRAW      void app_redraw(void)        draw the whole screen again at the next frame (the mirror lost a part)
 *   APP_MIRROR_TEST int app_mirror_test(int on)  the mirror's remote asks for a test session (on 1, off 0): 1 done
 *   APP_CSTART      void app_cstart(void)         first thing at a start, before the RAM is cleared and with nothing
 *                   set up (registers only, no RAM): an app's own second core that a reset left running, held
 *                   (fm1-base's own, cpu2 = true, needs none of these)
 *   APP_FAULT       void app_fault(const fm1_crash_t *c)   a crash, before the crash screen (c: the record): an app's
 *                   own second core given up; or the crash is on that core (c->sp on its stacks): it stops there
 *
 * Screen mirroring (fm1.toml mirror = true: FM1B_MIRROR 1, device/mirror.c): the screen and the panel live on a
 * computer (base/web/mirror/, tools/fm1_mirror.py), and the panel played from it. Nothing for the app to call; an app
 * with APP_REDRAW gets back what a slow link dropped.
 *
 * Backup and restore over USB (fm1.toml backup = true: FM1B_BACKUP 1, device/backup.c; tools/fm1_backup.py): the
 * app's saved objects, all or nothing. The app's app.h:
 *   APP_BACKUP_OBJS {OBJ_PROJ, ...}   the objects, in order (not OBJ_PANEL: the calibration is the unit's)
 *   APP_BACKUP_MAGIC 0x...u           its image's magic (4 ASCII bytes, little-endian), APP_BACKUP_VERSION its format
 *   APP_BACKUP_WHAT "replaces all songs"   the restore question's second line
 * and these: int app_backup_playing(void) (1: no restore now, STOP FIRST), void app_backup_flush(void) (what is not
 * saved yet, saved: a backup starts), void app_backup_hold(int on) (no saving while a backup streams),
 * void app_backup_restored(void) (everything from the flash again) */
