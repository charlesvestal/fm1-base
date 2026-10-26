<!-- SPDX-License-Identifier: MIT OR MPL-2.0 -->
<!-- Copyright (c) 2026 Julius Vitkauskas -->
# Open BLE test fixtures

- `r4-bonds-object.bin`: a synthetic FMOB v1 / FLBD v1 `OBJ_BLE` payload (1,108 bytes) in the layout written by the
  first open-stack store format (rounds R1 to R4). Two identities, both security directions with LTK/IRK/CSRK, SC
  and legacy, both CCCDs, GATT generations 3 and 0, valid WLSD seed sequence 17. All key bytes are deterministic
  test data; no device contents were read. Used by `tests/ble_bond_migration_test.inc` (v1 to v2 migration).
- `r4-store/`: that format's store parser (`ble_store.c`, `ble_store.h`), kept unmodified. Used only by the
  downgrade test in `tests/ble_open_store_test.c` (built by `tests/run_ble_s3_tests.py --s31 --open-store`), which
  checks that the old parser reads the current FMOB v2 object as empty instead of misreading keys. Never linked into
  a firmware.
