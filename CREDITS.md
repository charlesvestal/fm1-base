# Credits

- **Leo Kuroshita** ([@kurogedelic](https://github.com/kurogedelic), [Hügelton Instruments](https://hugelton.com)):
  Felucca, the firmware this base grew from, and felucca-bsp, which it is built on. Much of fm1-base is his code,
  used under MPL-2.0 with his permission ([hugelton/felucca-bsp#4](https://github.com/hugelton/felucca-bsp/issues/4)).
- **Charles Vestal** ([@charlesvestal](https://github.com/charlesvestal)): fm1-base, and the platform layer of the
  FM-1 firmwares it comes from (X0X, FoMni-1, CTL-1 and the others).
- **keremimo** ([@keremimo](https://github.com/keremimo)): the MIDI clock and transport sync in felucca-bsp's
  `usb.c` ([hugelton/felucca-bsp#3](https://github.com/hugelton/felucca-bsp/pull/3)).
- The idea of running FM-1 firmware in the browser: [Groove OS](https://www.groove-os.com/) by Peter Gombos (no
  code from it).
- **Julius Vitkauskas** ([@jvitkauskas](https://github.com/jvitkauskas)): the open Bluetooth LE stack
  (`ble = "open"`): the clean-room WL82 controller, BLE-MIDI, the NimBLE port, bond storage and its tests.
- [Apache NimBLE](https://github.com/apache/mynewt-nimble) (The Apache Software Foundation and the NimBLE
  contributors, Apache-2.0) and TinyCrypt (Intel Corporation, BSD-3-Clause; its ECC code by Kenneth MacKay,
  BSD-2-Clause): the open stack's host and cryptography, in `third_party/nimble/`.
- JIELI (Zhuhai Jieli Technology Co., Ltd.): the radio table values in `open_ble/hal/fm1_ble_tables.h`, from the
  AC79 SDK (Apache-2.0).
- Fonts: Barlow by The Barlow Project Authors and Terminus by Dimitar Toshkov Zhekov (both SIL OFL 1.1).
