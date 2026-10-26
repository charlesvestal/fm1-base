# SPDX-License-Identifier: MIT OR MPL-2.0
# Copyright (c) 2026 Julius Vitkauskas
"""Shared source/configuration list for target and radio-free integration tests."""
from pathlib import Path
import os
ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'third_party/nimble'
def includes():
    return [ROOT/'open_ble/port',ROOT/'open_ble',BASE/'nimble/include',BASE/'nimble/host/include',BASE/'nimble/transport/include',BASE/'porting/nimble/include',BASE/'nimble/host/util/include',BASE/'nimble/host/services/gap/include',BASE/'nimble/host/services/gatt/include',BASE/'ext/tinycrypt/include']
def sources():
    port=[BASE/'porting/nimble/src'/n for n in ('os_mbuf.c','os_mempool.c','os_msys.c','mem.c','endian.c')]
    host=[p for p in sorted((BASE/'nimble/host/src').glob('*.c')) if p.name!='ble_gatts_lcl.c']
    # 1.10 SM calls Mbed TLS. The private source adapter retains the offline
    # TinyCrypt backend; no external library or additional vendor patch.
    crypto=([ROOT/'open_ble/ble_sm_crypto.c']+[BASE/"ext/tinycrypt/src"/n for n in ("aes_encrypt.c","cmac_mode.c","ecc.c","ecc_dh.c","sha256.c","utils.c")] if os.environ.get("FELUCCA_BLE_STAGE","3")=="3" else [])
    return crypto+port+host+[BASE/'nimble/host/services'/svc/'src'/f'ble_svc_{svc}.c' for svc in ('gap','gatt')]+[BASE/'nimble/transport/src/transport.c']
def flags(target=False,vendor=True):
    opts=(['-Wno-missing-field-initializers','-Wno-sign-compare','-Wno-unused-parameter'] if vendor else [])+['-include',str(ROOT/'open_ble/port/felucca_nimble_config.h')]
    if not target: opts+=['-DFELUCCA_BLE_STAGE='+os.environ.get('FELUCCA_BLE_STAGE','2')]
    if target: opts+=['-I'+str(ROOT/'open_ble/port/libc')]
    return opts+['-I'+str(p) for p in includes()]
