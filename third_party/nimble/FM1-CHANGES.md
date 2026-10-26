<!-- SPDX-License-Identifier: MIT OR MPL-2.0 -->
<!-- Copyright (c) 2026 Julius Vitkauskas -->
# fm1-base integration changes

The upstream subset and pin are recorded in PINNED.md. Original NimBLE and
TinyCrypt license/copyright notices remain intact.

`ext/tinycrypt/src/ecc.c` has one conditional integration change: with
`FM1B_BLE_OPEN`, `muladd` calls the out-of-line `ble_wl82_mul32` in
`open_ble/ble_crypto.c`. The pure multiply consumes its product before the
next memory load. This follows felucca-bsp's documented pi32v2 hazard in
`bsp/docs/CPU.md`; final disassembly passes the existing multiply/load check.
Apart from the diagnostic hooks listed below (in `nimble/host/src/ble_sm.c`,
`ble_gap.c`, `ble_att_svr.c` and `ble_gatts.c`), all NimBLE files are
byte-identical to the 1.10.0 pin. The retained TinyCrypt files match 1.9.0,
except for this multiply hook. A diff against the two tags shows exactly these
five files.
The conditional additions are Copyright (c) 2026 Julius Vitkauskas,
MIT OR MPL-2.0; the original TinyCrypt BSD notices still apply.

The NPL and configuration live outside this vendor directory, in
`open_ble/port/` and `open_ble/ble_npl.c`. No NPL interface change was needed.
The privacy cache reset at IRK generation and the repeated-pairing fixes remain
application callbacks. NimBLE 1.10 now also resets its privacy cache during
host Reset. The removed `ble_sm_alg_ecc_init` call is no longer needed: 1.10
passes its RNG directly to each ECC call.

1.10's SM crypto source calls Mbed TLS instead of TinyCrypt and upstream removes
`ext/tinycrypt/`. Keep the existing TinyCrypt source and licenses because Mbed TLS
was not among the sources available to this port. The private source adapter
in `open_ble/ble_sm_crypto.c` and `open_ble/port/mbedtls/` handles only AES-128
encryption, one-shot CMAC, fixed-size integer serialization and uncompressed
P-256 operations used by `ble_sm_alg.c`. It is not Mbed TLS or its general API
or ABI. Unsupported modes, lengths, groups, points and scalars fail. It uses
fixed storage, wipes its private contexts, scopes/restores the TinyCrypt RNG,
and takes shared-secret projective blinding entropy from the supplied RNG.
The existing ECC multiply hook covers these operations too. No upstream SM
algorithm was replaced or patched for this backend. The settings 1.10 added
are set explicitly in `open_ble/port/fm1_nimble110_config.h`, which disables
the unused ones.
The software-revision DIS value now identifies NimBLE 1.10.0.

Metadata-only hooks are compiled under `FM1B_BLE_DIAG && FELUCCA_BLE_STAGE >= 3`:
`ble_sm.c` records the received Pairing Request and actual result/method/
association algorithm; `ble_gap.c` records the L2CAP update response;
`ble_att_svr.c` records rejected protected reads; `ble_gatts.c` records actual
CCCD writes with requested value and handler result. Subscription callbacks in
this pin also fire for reads, so they cannot accurately log writes. The hooks
call `ble_host_event` in main context, contain no key material, and are removed
from diagnostics-off builds. They do not alter protocol or permission behavior.
