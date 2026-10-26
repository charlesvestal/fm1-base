Apache NimBLE 1.10.0, tag nimble_1_10_0_tag
Commit a7a156f28954819e158b62dd613008f22f9cf73b
Offline source subset; original Apache-2.0 notices retained.
No NimBLE controller is compiled. See open_ble/port for the FM-1 NPL.

TinyCrypt is retained from NimBLE 1.9.0 (nimble_1_9_0_tag), commit
da7e3256da3ba80b232df880f40d8359311cc62e: headers, aes_encrypt, cmac_mode,
ecc, ecc_dh, sha256, utils, AUTHORS and LICENSE. Its original BSD-3-Clause /
MacKay notices remain intact. Upstream 1.10 removes TinyCrypt and requires
external Mbed TLS, which was not among the sources available to this port.
open_ble/ble_sm_crypto.c and open_ble/port/mbedtls/ implement only the SM calls
used by this pin on the retained TinyCrypt backend; they are not Mbed TLS.
See FM1-CHANGES.md for the local changes and docs/OPEN-BLE.md for the tests.
