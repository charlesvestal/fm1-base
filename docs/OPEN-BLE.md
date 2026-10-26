<!-- SPDX-License-Identifier: MIT OR MPL-2.0 -->
<!-- Copyright (c) 2026 Julius Vitkauskas -->
# Open Bluetooth LE (`ble = "open"`)

An open Bluetooth LE stack for the FM-1 (JieLi AC79/WL82): a clean-room link-layer controller for the WL82 radio,
Apache NimBLE 1.10.0 as the host, and a BLE-MIDI service. It is an opt-in alternative to the JieLi-based
`ble = true`, which stays the template's default. The application links nothing of JieLi's except a few
transcribed radio tables (Apache-2.0, see [License and provenance](#license-and-provenance)).

**Status.** One **peripheral** BLE-MIDI link to a computer or phone, verified on hardware with macOS, iOS and Linux
BlueZ ([Verification](#verification)): legacy and LE Secure Connections Just Works pairing, eight bonds (LRU),
encrypted reconnects, peer RPA resolution, central-initiated connection updates, ATT MTU up to 517, an opt-in
pairing-free mode, sender-timed MIDI clock. Works with `cpu2` and `usb_host`; in-app update, backup and restore
work while connected. **Not yet:** the central role (`ble_central`), so `TO_BLE_DEV` has no destination and
`plat_ble_scan`/`plat_ble_found`/`plat_ble_connect` do nothing. Bluetooth with WiFi stays a build error. The host
simulator and browser emulator run with Bluetooth off.

## Using it

```toml
ble = "open"
ble_central = false         # required
ble_diag = true             # default; false drops the detailed diagnostics (see Sizes)
ble_no_pairing = false      # default: pairing and encryption required
# cpu2 = true               # the second core as an audio worker; Bluetooth stays on CPU 0
# usb_host = true           # USB MIDI host when the app asks
```

Or once, for the unchanged template: `python3 tools/build.py template --open-ble [--ble-diag off]` (writes
`template/build/template-open.fwsc`; overrides `ble` and turns `ble_central` off). The build needs the JieLi
toolchain as before, and `$AC79_SDK` only for the three package-data files every package carries.

| Option | With `ble = "open"` |
| --- | --- |
| `ble_central = true` | build error: `ble = "open" supports peripheral BLE-MIDI only` |
| `ble_no_pairing` | works on its own (the vendor stack needs `ble_trim` with it) |
| `ble_trim`, `ble_heap_kb`, `ble_stack_kb` | ignored, with `open BLE: OPTION not used by the open stack`: it is peripheral-only already, its heap and NimBLE pools are fixed (`open_ble/ble_heap.c`, `open_ble/port/`), and it has no tasks of its own |
| `cpu2`, `cpu2_fallback`, `usb_host`, `backup`, `mirror` | unchanged |

**Application interface.** Apps keep plat.h as it is:

- `TO_BLE_HOST` and `TO_BLE` go to the peripheral link; `TO_BLE_DEV` queues nothing and has no room. Input arrives
  on `PORT_BLE` with `plat_midi_in_from() == TO_BLE_HOST`. `plat_midi_out_room(to)` is the minimum over the
  selected listening links (a subscribed link with a full queue gives 0; an absent one is ignored).
- `plat_ble_state()`: `BLE_OFF`, `BLE_STARTING`, `BLE_READY` (host synchronised), `BLE_FAILED`.
  `plat_ble_linked()`: a central is connected; `plat_ble_connected()`: it has also subscribed to MIDI.
  `plat_ble_enable(0/1)` ends the link and stops advertising, or advertises again (applied by the main loop, so
  safe from any context). `APP_BLE_AT_BOOT` decides the power-on start, as before.
- `OBJ_BLE` needs an 8 KiB A/B pair in `APP_STORE_MAP`, as for the vendor stack. The formats differ: switching
  stacks drops the bonds.
- An app's `plat_store_save` and `plat_data_erase`/`plat_data_write` pause advertising for the write, and return -8
  while a link is up (the link is kept; retry after it ends).
- MIDI is the usual four-byte USB-MIDI event form, SysEx and real-time included. Queues: TX 128 packets, RX 512,
  clock RX 128.

## Pairing and bonds

By default the MIDI characteristic and its CCCD need encryption and notifications wait for it; discovery is
public. The FM-1 sends no Security Request: an unencrypted MIDI read returns *Insufficient Authentication*, the
central pairs (Just Works, bonding, 16-byte keys, SC with legacy fallback), reads MIDI (empty) and subscribes. Just
Works encrypts but does not authenticate the person at the central or stop an active man-in-the-middle.

- **macOS:** Audio MIDI Setup → Window → Show MIDI Studio → Bluetooth, then Connect next to **FM-1 Open BLE**.
- **iOS:** an app's Bluetooth MIDI page (GarageBand: Settings → Advanced → Bluetooth MIDI Devices; MIDI Wrench),
  tap **FM-1 Open BLE**, accept pairing.
- **Linux:** `bluetoothctl` pair, trust, connect (BlueZ built with MIDI support gives an ALSA port).

**The bond is saved when the link closes.** An erase or program stops execution from flash and holds off the
radio's interrupts for up to hundreds of milliseconds, which a live link does not survive, so a new bond or a
changed subscription is written after the link closes, before advertising resumes. **Switching the FM-1 off while
still connected after first pairing loses the bond**; the central then fails to reconnect until the FM-1 is
forgotten there and paired again. So after pairing, **disconnect once, then reconnect**: on macOS turn Bluetooth
off and on ("Disconnect" in Audio MIDI Setup does not close the BLE link); on iOS likewise. `bond_pending 1` in
the USB readout means a bond is still waiting; a failed write is retried while idle. An update or restore saves a
pending bond before it starts.

Eight bonds, keyed by identity address (peer RPAs resolve through a 10-entry list). A ninth replaces the least
recently used one not connected (`bond_replacements`). Repeated sessions, or subscribing back to the same state,
write nothing. Forgetting the FM-1 on a phone and pairing again replaces that peer's keys.

**`ble_no_pairing = true`:** MIDI and its CCCD need no encryption, SMP answers **Pairing Not Supported (0x05)**,
no bonds or CCCDs are loaded or stored (the subscription lasts for the connection; bonds stored earlier come back
when pairing is on again). Anyone nearby can connect; MIDI is plaintext and unprotected against injection. After
switching, forget the FM-1 on a central that keeps trying to encrypt.

**GATT database.** GAP 0x0001..0x0005 (Device Name value 0x0003 `FM-1 Open BLE`, Appearance 0x0005 = 0); GATT
0x0006..0x000d (Service Changed 0x0007/0x0008/0x0009, Server/Client Supported Features 0x000b/0x000d); MIDI
0x000e..0x0011 (0x000f/0x0010/0x0011); Device Information 0x0012..0x001a (not advertised: `M-VAVE`, `FM-1`,
firmware and software revision). Tests assert these handles: append new services after DIS, insert nothing before.
`BLE_GATT_DB_VERSION` (`open_ble/ble_bonds.h`, now 3) is stored per bond; bump it when services, handles,
properties or permissions change. A bonded peer with an older generation that enabled Service Changed gets one
full-range indication after encryption and becomes current only on confirmation; new bonds never get one.
Database Hash / Robust Caching is not implemented (it would move the handles; this NimBLE pin lacks it).

**Link parameters and advertising.** Connectable `ADV_IND` on 37/38/39 every 20 ms with flags and the MIDI UUID;
the name in the scan response; no TX Power Level (calibrated dBm unknown). At least 5 s after connecting and after
pairing, the FM-1 asks once, over L2CAP, for 15 ms, latency 0, 6 s timeout (BLE-MIDI and Apple's guidelines);
busy is retried after 1 s. The central decides; 7.5 to 30 ms all work. Local ATT MTU 517 (a Mac asks 185); no
data length extension.

## Update, restore and backup with Bluetooth on

The open flash driver refuses every erase and program while the radio advertises or is linked (-8). Three jobs
therefore hold Bluetooth quiet throughout (`flash_quiet_begin`/`_end`, `device/ble_open.c`):

| Job | Hold starts | Link ended with | Afterwards |
| --- | --- | --- | --- |
| In-app update (web installer, `tools/fm1_install.py`) | before the session | 0x15 (Power Off) | restart into the new firmware; a refused or failed update gives Bluetooth back at once |
| USB restore (`backup = true`) | once the image's header checks out | 0x13 (Remote User Terminated) | as the app wants (`plat_ble_enable`; a change meanwhile wins) |
| Backup, only if unsaved changes must be saved during a link | around that save | 0x13 | right after the save; the image streams with Bluetooth on |

The link ends properly (LL_TERMINATE_IND, then the central's acknowledgement or supervision timeout), so the
central reconnects by itself later. The hold waits up to 7 s (above the 6 s timeout the FM-1 requests, below the
hosts' 8 s wait for the update's next request), saves a pending bond, and keeps advertising off until the job ends.
If the radio is still busy at 7 s (a silent central with a timeout above 7 s), the job's writes are refused:
FAILED or NOT RESTORED, nothing changed; retry once the link has timed out. A backup with nothing unsaved keeps
the link; ordinary app saves never end one. While a job holds it, `plat_ble_state()` is `BLE_OFF`. UBOOT/REC and
safe-mode updates and boot-time writes run before Bluetooth starts.

## MIDI clock smoothing

BLE-MIDI carries a 13-bit millisecond sender timestamp. The decoder keeps each message's timestamp (across
interleaved real-time bytes and wrap). Clock, Start, Continue and Stop (F8, FA, FB, FC) go through a deadline
queue; all other MIDI is delivered immediately.

- The quickest arrival maps the sender's clock onto uptime; the largest relative lateness of the last 1 to 2 s
  (two one-second peak buckets) is the allowance. A message is released at its mapped time plus the allowance, held
  at most **120 ms**; later ones go at once. 4096 ms of silence, or a new connection, starts the estimate over.
- A sender that stamps every clock in a packet alike cannot be smoothed (the log shows zero gaps). Slow app
  polling and USB enumeration can still delay consumption.

`tools/fm1_remote.py log` prints `BT clk N gap MIN..MAX z Z late MAX/ALLOW`: clocks since the last read, sender gaps
(ms), zero gaps, worst relative lateness and the allowance. It describes the sender and arrival, not output spacing.

## Sizes

The template, same BSP and toolchain, bytes. "Vendor" is the template's default `ble = true, ble_central = true`
(which also has a central link).

| Build | App flash | `.data`+`.bss` (of 98,304) | Pool (of 344,064) | RAMTEXT code |
| --- | ---: | ---: | ---: | ---: |
| No Bluetooth (`--no-ble`) | 81,984 | 27,908 | 69,384 | 3,172 |
| Vendor | 219,000 | 76,420 | 93,960 | 7,060 |
| Open, diagnostics (default) | 212,180 | 34,728 | 155,772 | 3,244 |
| Open, `ble_diag = false` | 191,220 | 33,844 | 100,796 | 3,244 |
| Open, `ble_diag = false`, `ble_no_pairing = true` | 166,864 | 33,844 | 100,236 | 3,244 |

Against the vendor build, `ble_diag = false` saves 27,780 bytes of flash and 42,576 of static RAM for 6,836 more
pool (35,740 bytes less RAM in all). Diagnostics add 20,960 bytes of flash, 884 of static RAM, 54,976 of pool and a
separate 20,480-byte RAMTEXT trace. Every package is 611,498 bytes (fixed container layout).

## Architecture

| Files | Role |
| --- | --- |
| `open_ble/ble_logic.c`, `ble_controller.c`, `ble_link*.c`, `ble_security.inc`, `ble_privacy.c`, `ble_timing.c`, `ble_health.c` | the controller: calibration decoding, in-memory HCI, peripheral link layer, CCM, resolving list, resumable work and budgets, health records |
| `open_ble/ble_crypto.c`, `ble_seed.c`, `ble_sm_crypto.c` | AES/CCM glue, boot seed, the SM crypto adapter |
| `open_ble/ble_bonds.c`, `ble_bond_capacity.inc`, `ble_store.c` | bond cache and the atomic `OBJ_BLE` snapshot |
| `open_ble/ble_host.c`, `ble_midi.c`, `ble_npl.c`, `ble_heap.c`, `port/` | NimBLE application and OS layer, BLE-MIDI codec and clock queue, heap, configuration |
| `open_ble/hal/` | radio, DMA, TIMER0 executor, locks, AES engine, calibration reads, radio tables, flash settings |
| `device/ble_open*.c`, `ble_open_isr.inc`; `unity.c`, `main_fm1.c`, `plat_fm1.c`, `app.ld` | the fm1-base adapter (lifecycle, plat.h, flash-job hold, USB log, interrupt entries); boot phases, storage and MIDI integration, memory sections |
| `third_party/nimble/`, `tools/nimble_build.py` | NimBLE and TinyCrypt; the source list and flags shared by firmware and tests |

`open_ble/hal/` is the only code outside the BSP allowed to touch registers; the build's register check enforces
that for `device/`, `include/`, `lib/` and apps (a negative fixture tests it), and open builds skip the unlinked
`device/radio.c`.

**Interrupts.** All of Bluetooth runs on CPU 0 (with `cpu2`, the second core runs only audio jobs from RAM and
must never call MIDI, Bluetooth, locks or flash writes). Level 7: CPU exception. 5: baseband RX and event close
(lines 29, 45). 4: TIMER5 (panel scan, USB/TRS, the app tick). 3: ALNK0 audio. 2: TIMER0, the link-layer executor
(never shared with an app). Main: NimBLE, SMP/ECC, bond commits, the app. Handlers above the executor are wrapped
with nested-preemption accounting, which the executor subtracts. Budgets: dispatch ≤ 7,500 µs, the executor's own
work stops after 250 µs, an item ≤ 100 µs, DMA refill ≤ 80 µs; overruns are counted. TIMER4 gives 24 MHz ticks,
the BSP's `fm1_ms` the host clock. No NimBLE code runs in an interrupt; the main loop and LCD yields poll the host
(reentrancy-guarded). With `usb_host`, the BSP's main-context USB-host waits poll the host at most once per ms
(no USB registers, no app code); app consumption can still pause during hub enumeration. Flash windows hold off
levels up to 6 (`FL_IRQ_MASK_LEVEL 6`), and writes are refused unless the radio is idle and unlinked, so a live
link never sees an XIP blackout.

**Boot.** P2 (before LCD/audio/USB): read calibration, load `OBJ_BLE`, commit the next boot seed. P3: attach the
controller vectors, masked. P4 (only if Bluetooth is wanted): radio bring-up, one-time calibration (audio
silenced), a TIMER0 self-test (one entry within 1 ms), NimBLE; a failure gives `BLE_FAILED`. Off/on keeps the
radio and NimBLE initialised; stopping is non-blocking (20 to 100 ms bound, then the radio is quarantined). A
crash or watchdog reset during a Bluetooth run skips the automatic start at the next boot (`plat_ble_start()`
still works); clean updates and power-on do not. The 48-line log ring and crash breadcrumbs survive warm resets.

**Persistence.** `OBJ_BLE` is an fm1-base A/B object: the inactive copy is erased, the payload written from +256,
the CRC-protected header last, then verified; loading falls back to the other copy. The last 256 bytes stay blank,
so a copy never looks like an update record to the loader. Payload, 1,236 bytes: 0..15 `FMOB` header (version 2,
capacity 8, stride 144, replacement counter); 16..79 the 64-byte `WLSD` boot seed; 80..1231 eight 144-byte peers
(identity, presence bits, MRU rank, GATT generation, both directions' keys: EDIV, Rand, LTK, IRK, CSRK, two
CCCDs); 1232..1235 CRC-32.

- The development builds' v1 object (1,108 bytes) is converted in RAM and written as v2 with the next save; a v1
  parser reads v2 as empty, never as keys. No raw NimBLE structures or private addresses are stored.
- Failed commits stay pending; a read error while choosing the copy refuses the save before erasing.
- Wear: one erase per boot plus one per bond change (about 200,000 commits at a nominal 100,000 cycles per
  sector; arithmetic, not qualification). A power cut keeps the old or the new snapshot whole. Restoring an older
  `OBJ_BLE` rolls the seed chain back: device-local persistence, not secure storage. Seed entropy is tracked by
  class and credit; clock and RF diversifiers get none; no CSPRNG qualification is claimed.
- Calibration is read from the first VM sector `0x93000..0x93FFF` and BTIF `0xE9000..0xE9FFF`; both are
  read-only in open builds, also through `plat_data_*` (they overlap XDATA0/1). Overwritten calibration means
  defaults; without a valid factory identity address startup fails. `0x94000..0x96FFF` (the vendor stack's seed
  and bond sectors) is left to the app.

## BSP interface

The open build needs a felucca-bsp change, proposed upstream separately; until it is merged `bsp` points at
`jvitkauskas/felucca-bsp` branch `open-ble-pr` (on upstream `2cd696a`). It names no Bluetooth code:

- **Fix for every firmware:** `chip/fm1_flash.h` no longer returns to XIP while an erase or program is still busy.
  `fl_wait_issued` (`chip/fm1_flash_wait.h`) waits from the command until idle: 0 within 500 ms / 10 ms,
  `FL_ERR_LATE_READY` (-4 by default) after, and a chip reset from RAM if still busy at 2 s / 50 ms.
- **Hooks, compiled to nothing unless defined:** `FL_IRQ_MASK_LEVEL` (7 = `cli`, default; 6 = clear ICFG bit 8,
  nests, leaves only the CPU exception running), `FL_CAN_START()` (asked once per sector erase and page program
  with the window's interrupts held off; refusal is `FL_ERR_REFUSED`, -8), `FL_ERR_LATE_READY`, and the existing
  `FL_RANGE_OK`. In `chip/fm1_isr.S`, `FM1_ISR name, body[, enter, leave]`, and `-DFM1_ISR_ENTRIES="file"` to
  replace the default entries.

fm1-base sets them in `open_ble/hal/fm1_ble_flash.h` (included by `device/unity.c` before `fm1_flash.h`, guarded
by `#error`): the calibration exclusion, level 6, the gate `fm1_ble_flash_allowed()`, late code -9, plus the store
read `fl_read_plain`. `device/ble_open_isr.inc` lists the TIMER5, audio, baseband RX and event entries with
`ble_time_preempt_enter/leave`, and the unaccounted TIMER0 executor. `tools/build.py` assembles `fm1_isr.S` with
them, and the second core's build ID hashes `.inc` sources too.

## Diagnostics

```sh
python3 tools/fm1_remote.py --port "TEMPLATE FM-1:TEMPLATE FM-1 MIDI 1 24:0" ble   # compact
python3 tools/fm1_remote.py --port "..." log      # crash record, log ring, everything
```

Choose the exact **USB** port (a BlueZ BLE-MIDI port may also say "FM-1"); `--port` comes first. A readout must end
with its `end` frame (three tries of 8 s). Both work with `ble_diag = false`.

| Lines | Meaning |
| --- | --- |
| `peripheral_state`, `_linked`, `_connected`; `enc_enabled`, `nimble_bonded`, `nimble_subscribed`, `nimble_mtu` | lifecycle, link, security, subscription |
| `conn_interval_1250us`, `conn_latency`, `conn_timeout_10ms`, `param_*` | parameters; the FM-1's update request |
| `disconnects`, `last_disconnect_reason`, `supervision_fail`, `mic_failures`, `late_instants` | link losses |
| `bond_pending`, `bond_count`, `bond_capacity`, `bond_replacements`, `bond_failures`, `bond_identity ...` | bonds; one `bond_identity ADDR type T rank R SC\|legacy cccd H=F ...` row each (rank 1 newest; 0008 Service Changed, 0010 MIDI; 0002 indicate, 0001 notify); no keys |
| `midi_packets_in/out`, `midi_ble_in/out`, `midi_bad`, `midi_drop`, `BT clk` | MIDI traffic, losses, clock timing |
| `executor_*_over_*`, `refill_over_80us`, `worker_timer_over_7500us`, `worker_gap*` | budget overruns; `worker_gap` rows: gaps above 7.5 ms with state and overlapping causes (boot, radio start/stop, flash) |
| `audio_late`, `audio_xruns`, `audio_max_wall_us`, `dma_canary_errors` | audio coexistence, DMA guard |
| `adv_session`, `connect_seen`, `connect_accepted` | this advertising session: CRC-valid CONNECT_IND seen, links made |

Diagnostics builds also print the last 32 `ble_event` rows (connect, disconnect reason, encryption, SMP request
and result, MTU, CCCD writes, Service Changed, L2CAP request and result, connection updates, ATT rejects), without
keys or private addresses. `ble_diag = false` drops the trace, dumps, snapshots,
event ring and detailed counters, and keeps budget enforcement, crypto self-checks, canaries, health and MIDI
counters, the four-entry gap ring and the bond rows.

Linux bench tools (root or CAP_NET_RAW, BlueZ, `btmon`, a bonded FM-1; they flash nothing and keep bonds):
`tools/ble_conn_update_bench.py --address A --usb-port P --output build/ble-bench` cycles 15/7.5/11.25/15/30/15 ms
and a reconnect; `tools/ble_overnight_soak.py ... --midi-port M --hours 8 --output build/ble-soak` plays notes,
changes parameters every 10 minutes, samples every minute and writes `summary.json`.

## Tests

```sh
python3 tests/run_open_ble_tests.py              # all open BLE suites (--seams-only: the quick ones)
BLE_MIC_EVENTS=10000 python3 tests/run_open_ble_tests.py   # shorter randomized stress (default 10,000,000)
python3 tests/open_ble_build_test.py             # build policy (needs an open build's .symbols/.map)
tests/run_tests.sh                               # base suite; PKG=X.fwsc runs the update path on X ($AC79_SDK)
python3 tools/build.py template --open-ble --host && \
  template/build/host/template-open_host template/tests/open.txt template/build/host
```

The open BLE suites need no hardware, SDK or JieLi toolchain; their C code runs under ASan/UBSan with leak
checking, against the real sources (functions are cut verbatim from `device/` and `open_ble/hal/` where needed):

- seams: MIDI routing and room, the advertising HAL on a fake register bus, health records, CPU2 and USB-host
  adapters, locks, the clock queue (7.5/15/30 ms, 120/140 BPM), the flash-job hold and settings, the bench tools;
- the controller's development stages S1 to S3.1 over a simulated radio with the real NimBLE host: legacy, SC,
  RPA, repeated and forgotten-then-repeated pairing, bond capacity, connection updates, Service Changed;
- the store: power cuts at each write, v1 migration and the v1 parser's downgrade
  (`tests/fixtures/open_ble/`);
- MIC stress and reliability: 5M pings at 15 ms, 5M at 30 ms and 10M randomized encrypted events per profile,
  22 directed fault cases; NimBLE 1.10 crypto/GAP/ATT regressions; `ble_no_pairing`;
- flash jobs: NimBLE and a test central with `bsp/chip/ota.c`, `device/backup.c`, `storage.c` and the device glue
  on a NOR model that refuses busy writes: update, backup and restore while advertising and linked, terminate
  reasons, the bond across an update, silent centrals.

Outputs go to `build/` (git-ignored). The only sanitizer exception is TinyCrypt `aes_encrypt.c`'s shift-base. For
the base suite under sanitizers, set `CC` to a wrapper script running
`cc -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie "$@"`.

## Verification

On a real FM-1, October 2026. Diagnostics build:

| Central | Result |
| --- | --- |
| macOS (Audio MIDI Setup) | bonded reconnect without a pairing prompt; MIDI both ways |
| Linux BlueZ, Intel AX210, LE Secure Connections | bonded reconnect without a pairing prompt; MIDI both ways |
| iPhone (GarageBand receiving, MIDI Wrench sending) | bonded reconnect without a pairing prompt; MIDI both ways |
| iPhone, `ble_no_pairing = true` | no prompt; MIDI both ways; no bonds stored |
| any, `cpu2 = true` build | the second core runs jobs; BLE MIDI both ways |

MIDI clock: plain 120 BPM (1,440 ticks); REAPER at 140 BPM (1,798 ticks, distinct timestamps, worst lateness 42 ms,
within the learned allowance).

Overnight soak, release build, a Mac as central sending a note a second: 8 h (23:01 to 07:01); 56,614 notes sent
and 56,614 received; one connection all night (0 disconnects); 0 MIC or supervision failures; 0 dropped or bad
MIDI; 0 audio xruns; 0 executor items over 100 µs.

Current build, the Mac connected and playing:

| Action | Result |
| --- | --- |
| Backup, nothing unsaved | link kept |
| Backup, an unsaved change | link ended (local terminate acknowledged), saved, the Mac reconnected by itself |
| Restore, answered NO | nothing changed; the Mac reconnected |
| Restore, confirmed with SEL on the FM-1 | restored; the Mac reconnected; bonds intact |
| In-app update (`tools/fm1_install.py`, about 58 s), diagnostics → release and release → release | succeeded; the Mac reconnected by itself; bonds and settings kept |
| iPhone pairing | MIDI both ways |

(The restore question only takes the FM-1's physical SEL key.)

Rare encrypted-link drops seen in development came from the Linux desktop's Intel AX210, which sometimes sent
packets with a valid CRC and a wrong MIC; the FM-1 closed the link (0x3D) as the specification requires, and an
independent nRF sniffer saw the same bad MIC on air. Not an FM-1 fault.

## Known limitations

- Peripheral only; no Bluetooth with WiFi; Bluetooth off in the simulator and emulator.
- A bond is saved when the link closes; app saves and long app flash jobs (`plat_data_*`) are refused while
  linked. Saving while connected would need everything to run from SRAM (an experiment cost 152 KiB of pool).
- An update or restore while a silent central holds the link with a timeout above 7 s fails cleanly; a backup with
  unsaved changes briefly ends the link.
- Just Works only (no MITM protection); `ble_no_pairing` is plaintext. The FM-1 uses its public address (no local
  privacy). No TX Power AD, data length extension or Robust Caching.
- Clock smoothing is capped at 120 ms and needs distinct sender timestamps.
- Not measured on hardware: RF range and temperature, `usb_host` with Bluetooth during hub enumeration, stack
  margin, entropy quality. Android was not part of this verification.

## NimBLE and TinyCrypt

`third_party/nimble/` is a subset of NimBLE 1.10.0 (`a7a156f`; host only), unchanged but for metadata-only
diagnostic hooks in four host files (diagnostics builds only). See `PINNED.md` and `FM1-CHANGES.md` there.

NimBLE 1.10's security manager calls Mbed TLS, and upstream dropped TinyCrypt. Mbed TLS was not among the sources
available to this work, so the TinyCrypt subset NimBLE 1.9.0 shipped is kept (AES, CMAC, ECC/ECDH P-256, SHA-256).
Its one change: `ecc.c`'s multiply calls an out-of-line `ble_wl82_mul32` to avoid the CPU's multiply/load hazard
(`bsp/docs/CPU.md`; the build checks it). `open_ble/ble_sm_crypto.c` and `open_ble/port/mbedtls/` are a small
private adapter under Mbed TLS's names supplying exactly what `ble_sm_alg.c` calls (AES-128, one-shot CMAC,
uncompressed P-256); it is not Mbed TLS. No NimBLE SM algorithm is replaced; real Mbed TLS would replace only the
adapter.

## License and provenance

- This stack's own files (`open_ble/` except the radio tables, `device/ble_open*`, its tests, tools, fixtures and
  this document): **MIT OR MPL-2.0**, Copyright (c) 2026 Julius Vitkauskas (`LICENSES/MIT-BLE.txt`). Changes to
  existing fm1-base and BSP files are MPL-2.0 with their notices.
- `open_ble/hal/fm1_ble_tables.h`: **Apache-2.0**, values from the JieLi AC79 SDK, Copyright (c) JIELI (Zhuhai
  Jieli Technology Co., Ltd.). `br_seq` is the SDK's `wl_rfd_ram_lut` (`apps/common/net/wifi_conf.c`); the others
  are constant data of the SDK's compiled Bluetooth controller (`br_agc`) and Wi-Fi RF library, read from an
  SDK-built firmware image by the specification side. Vendor data, not independently derived; the header lists
  each table's source.
- NimBLE: Apache-2.0 (`third_party/nimble/LICENSE`, `NOTICE`). TinyCrypt: BSD-3-Clause, its ECC code BSD-2-Clause.
- The controller was implemented clean-room from a hardware specification written separately. fm1-base's `ble/`,
  `device/radio.c` and `examples/wifi/` (which wrap JieLi's closed libraries) were not consulted; the tables'
  author read no vendor binary, disassembly or library. One tooling exception: early builds ran the existing
  register checker, which reads every device C file including `device/radio.c`; open builds now skip that file.
- An open application links nothing of JieLi's but the tables; its package carries the three SDK package-data
  files like every fm1-base package. Factory calibration is read from the unit at run time.

See `LICENSING.md` and `CREDITS.md`.

## Roadmap

- **Central role** (BLE-MIDI keyboards and controllers, `TO_BLE_DEV`, `plat_ble_scan`): being specified for the
  controller.
- **Dual role** (peripheral and central at once, as the vendor stack's `ble_central`): after that.
