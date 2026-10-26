#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR MPL-2.0
# Copyright (c) 2026 Julius Vitkauskas
"""Offline BLE suites. Never contacts USB, Bluetooth, devices or the SDK."""
import os
import sys
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
os.chdir(ROOT)
out=ROOT/'build/open-seam-tests'
out.mkdir(parents=True,exist_ok=True)
s=os.environ.copy()
s['ASAN_OPTIONS']='detect_leaks=1:halt_on_error=1'
s['UBSAN_OPTIONS']='halt_on_error=1:print_stacktrace=1'
def run(*cmd):
    print('+ '+' '.join(map(str,cmd)),flush=True)
    subprocess.run(list(map(str,cmd)),check=True,env=s)
source=(ROOT/'device/plat_fm1.c').read_text()
a=source.index('static uint32_t midi_n[NPORTS]')
b=source.index("/* the keys' velocity",a)
(out/'midi_seam.inc').write_text(source[a:b])
hal=(ROOT/'open_ble/hal/fm1_ble.h').read_text()
def function(name,text=hal):
    import re
    match=re.search(r'^(?:static )?(?:int|void) '+name+r'\([^;]+?\)\n\{',text,re.M)
    assert match, name
    start=match.start(); end=match.end(); depth=1
    while depth:
        if text[end]=='{': depth+=1
        if text[end]=='}': depth-=1
        end+=1
    return text[start:end]+'\n'
(out/'hal_adv_seam.inc').write_text('\n'.join(function(n) for n in (
    'bb_map','bb_remap','bb_frame_publish_step','bb_payloads','bb_connection','bb_rx_pending','fm1_ble_adv_start'))+
    function('ble_port_reset_stats',(ROOT/'device/ble_open_platform.c').read_text()))
macros=subprocess.check_output([os.environ.get('CC','cc'),'-dM','-E','-DFM1B_BLE_OPEN=1',
    '-Ibsp/chip','-include','stdint.h','-include','open_ble/hal/fm1_ble_flash.h','-'],input=b'').decode()
(out/'flash_ranges.inc').write_text('\n'.join(line for line in macros.splitlines() if line.startswith('#define FL_'))+'\n')
flags=['-DBLE_HOST_TEST=1','-std=c11' ,'-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
       '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-DFM1B_BLE_OPEN=1',f'-I{out}']
for name in ('ble_open_seam_test','ble_open_midi_test','ble_open_ranges_test'):
    run(os.environ.get('CC','cc'),*flags,'tests/'+name+'.c','open_ble/ble_health.c','-o',out/name)
    run(out/name)
for diag in ('0','1'):
    run(os.environ.get('CC','cc'),*flags,'-DFM1B_BLE_DIAG='+diag,
        'tests/ble_open_health_test.c','open_ble/ble_health.c','-o',out/('health-'+diag))
    run(out/('health-'+diag))
for diag in ('0','1'):
    run(os.environ.get('CC','cc'),*flags,'-DFM1B_BLE_DIAG='+diag,
        'tests/ble_open_adv_test.c','open_ble/ble_logic.c','open_ble/ble_link_logic.c',
        'open_ble/ble_health.c','-o',out/('adv-'+diag))
    run(out/('adv-'+diag))
(out/'usb_wait_seam.inc').write_text(function('ble_usb_host_wait',(ROOT/'device/ble_open.c').read_text()))
host=(ROOT/'host/plat_host.c').read_text()
a=host.index('static int cpu2_on;'); b=host.index('\n#endif',a)
(out/'cpu2_host_seam.inc').write_text(host[a:b])
plat=(ROOT/'device/plat_fm1.c').read_text()
a=plat.index('static volatile uint8_t in_tick;'); b=plat.index('void plat_sysex_send',a)
(out/'lock_seam.inc').write_text(plat[a:b])
run(os.environ.get('CC','cc'),*flags,'tests/ble_platform_pr1_test.c','-o',out/'platform-pr1')
run(out/'platform-pr1')
run(os.environ.get('CC','cc'),*flags,'tests/ble_midi_clock_test.c','open_ble/ble_midi.c','-o',out/'clock')
run(out/'clock')
run('python3','tests/ble_conn_update_bench_test.py')
run('python3','tests/ble_soak_test.py')
if '--seams-only' in sys.argv:
    print('Open BLE platform/codec/tool seam suites passed.',flush=True)
    sys.exit(0)
run('bash','tests/run_ble_s1_tests.sh')
run('python3','tests/run_ble_s2_tests.py')
run('python3','tests/run_ble_s3_tests.py')
run('python3','tests/run_ble_s3_tests.py','--s31')
run('python3','tests/run_ble_s3_tests.py','--s31','--open-store')
run('python3','tests/run_ble_s3_tests.py','--s31','--open-store','--no-diag')
run('python3','tests/run_ble_s3_tests.py','--mic-stress')
run('python3','tests/run_ble_s3_tests.py','--s31','--open-store','--reliability')
run('python3','tests/run_ble_s3_tests.py','--mic-stress','--reliability')
run('python3','tests/run_ble_s3_tests.py','--s31','--open-store','--no-pairing')
run('python3','tests/run_ble_s3_tests.py','--s31','--open-store','--flash-jobs')
run('python3','tests/run_ble_s3_tests.py','--s31','--open-store','--flash-jobs','--no-diag')
print('All open BLE ASan/UBSan suites passed.',flush=True)
