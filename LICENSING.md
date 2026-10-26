# Licensing

**fm1-base is MPL-2.0** (full text in `LICENSE`), file by file: every source file carries an
`SPDX-License-Identifier: MPL-2.0` header (the open Bluetooth stack's own files and its vendored libraries, below,
say theirs). If you change one of these files and distribute it, publish your change to that file under MPL-2.0.
Your own files, such as an app built on fm1-base, may use any licence, closed ones included.
So a firmware made of felucca-bsp, fm1-base and its own code may link JieLi's binary Bluetooth and Wi-Fi libraries.

## Where the code comes from

| Code | Origin | Why it is MPL-2.0 |
| --- | --- | --- |
| Leo Kuroshita's code (from Felucca, through the firmwares below) | [Felucca](https://github.com/hugelton/Felucca), GPL-3.0-only there | his permission to use the Felucca code he owns under MPL-2.0: [hugelton/felucca-bsp#4](https://github.com/hugelton/felucca-bsp/issues/4) |
| Charles Vestal's code | the platform layer of [fm1-x0x](https://github.com/charlesvestal/fm1-x0x), [fm1-fomni](https://github.com/charlesvestal/fm1-fomni) and the other FM-1 firmwares, GPL-3.0-only there | his own |
| `bsp/` | [felucca-bsp](https://github.com/hugelton/felucca-bsp), a git submodule | MPL-2.0 there |
| `ble/` (the Bluetooth shim and the MIDI service; `ble/wifi`, the WiFi glue) | Charles Vestal's fm1-ble and fm1-radio, used by fm1-ctl | his own; a few parts repeat the JieLi AC79 SDK (below) |

Some files in `ble/` repeat parts of the JieLi AC79 SDK, Copyright (c) JIELI (Zhuhai Jieli Technology Co., Ltd.),
under the Apache License 2.0 (`LICENSES/Apache-2.0.txt`): the connection handling of `le_trans_data.c` in
`ble/midi/le_midi.c`, interface details in `ble/shim/shim.h` and `ble/shim/misc.c`, and the radio's calibration
values in `ble/shim/rf_const.c` (Apache-2.0 only). Each file's header says which parts, and carries
`MPL-2.0 AND Apache-2.0`.

The GPL copies in Felucca and in those firmwares stay under the GPL; the copies here are MPL-2.0. No code by other
Felucca contributors is in fm1-base: the history of every file was checked when it was imported (from fm1-fomni
`7e7547f`).

## The open Bluetooth stack (`ble = "open"`)

| Code | Licence | Notes |
| --- | --- | --- |
| `open_ble/` (except the radio tables), `device/ble_open*`, its tests, tools, fixtures and `docs/OPEN-BLE.md`, by Julius Vitkauskas | MIT OR MPL-2.0 (`LICENSES/MIT-BLE.txt`, `LICENSE`) | choose MPL-2.0 to follow fm1-base's per-file terms. His changes to existing fm1-base files are MPL-2.0, as those files are |
| `open_ble/hal/fm1_ble_tables.h`: radio table values from the JieLi AC79 SDK, Copyright (c) JIELI (Zhuhai Jieli Technology Co., Ltd.) | Apache-2.0 (`LICENSES/Apache-2.0.txt`) | the only JieLi content an open build links (below) |
| `third_party/nimble/`: Apache NimBLE 1.10.0 | Apache-2.0 (`LICENSE`, `NOTICE` there) | an unmodified subset but for diagnostic hooks and one multiply change, listed in `PINNED.md` and `FM1-CHANGES.md` |
| `third_party/nimble/ext/tinycrypt/`: TinyCrypt, from NimBLE 1.9.0 | BSD-3-Clause (Intel); the ECC files also BSD-2-Clause (Kenneth MacKay) | `LICENSE`, `AUTHORS` and the source headers there |

The tables' header carries this attribution:

> Values transcribed from the JieLi AC79 SDK, Copyright (c) JIELI (Zhuhai Jieli Technology Co., Ltd.), under the Apache License 2.0 (LICENSES/Apache-2.0.txt): br_seq is the SDK's wl_rfd_ram_lut (apps/common/net/wifi_conf.c); the other tables are values of the SDK's radio libraries.

`br_seq` is SDK source. The other tables are constant data of the SDK's compiled Bluetooth controller (`br_agc`)
and Wi-Fi RF library (the rest): the specification side of the clean room read them from an SDK-built firmware
image and recorded them in the hardware specification; the controller's author worked from that specification and
read no vendor binary. The clean room covers the code; these tables are vendor data used under the SDK's
Apache-2.0 licence, not independently derived. The SDK repository has a single top-level Apache-2.0 licence, with
no separate terms for its libraries.

An open build links no JieLi controller, host, crypto, RF library, shim, WiFi or lwIP code. Its package carries the
three SDK package files listed below, as data, like every fm1-base package.

## Not part of fm1-base

| What | Licence | Where |
| --- | --- | --- |
| Barlow Semi Condensed (The Barlow Project Authors) | SIL OFL 1.1 | `assets/fonts/BarlowSemiCondensed-*.ttf`, `assets/fonts/Barlow-OFL.txt` |
| Terminus (Dimitar Toshkov Zhekov) | SIL OFL 1.1 | `assets/fonts/ter-u*.bdf`, `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK files a package needs (`uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin`) | Apache-2.0 | read from your SDK checkout at build time; never in this repository |
| JieLi's Bluetooth libraries (`btctrler.a`, `btstack.a`, `wl_rf_common.a`, ...: binary only) and the few SDK sources and headers they need | Apache-2.0 | copied from your SDK checkout into `build/` by a build with `ble = true`; never in this repository |
| JieLi's WiFi libraries (`wl_wifi_sta_sfc.a`, `wpasupplicant.a`, `hsm.a`: binary only) and the SDK's lwIP 2.1.3 sources | Apache-2.0 (lwIP: BSD-3-Clause) | copied from your SDK checkout into `build/` by a build with `wifi = true`; never in this repository. `ble/lwip/lwipopts_sfc.h` and `ble/wifi/rf_moderate.c` are the SDK's (Apache-2.0), the first changed where marked "FM-1" |
| m-bootloader 1.2 (Julius Vitkauskas, github.com/jvitkauskas/m-bootloader): its FM-1 installer package and bootloader, which the web installer offers to install | MIT | `web/m-bootloader/` (with its `LICENSE`); a site copies the package and the licence into `firmware/` |
| The browser emulator's voice: eSpeak NG, through fm1-reader | GPL-3.0-or-later | taken from an fm1-reader checkout when the emulator is built; a separate program the page loads |
