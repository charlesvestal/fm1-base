#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR MPL-2.0
# Copyright (c) 2026 Julius Vitkauskas
import os,sys,subprocess
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
os.environ['FELUCCA_BLE_STAGE']='2'
import nimble_build as nb
os.chdir(nb.ROOT)
out=nb.ROOT/'build/s2-tests';out.mkdir(exist_ok=True,parents=True)
common=['-DBLE_HOST_TEST=1','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie']
cc=os.environ.get('CC','cc')
def compile(p,flags=()):
    obj=out/(str(p).replace('/','_')+'.o')
    subprocess.run([cc,*common,*flags,'-c',str(p),'-o',str(obj)],check=True)
    return obj
base=[compile(Path('open_ble')/(n+'.c')) for n in ('ble_logic','ble_controller','ble_link_logic','ble_link','ble_midi','ble_heap','ble_timing')]
sim=compile(Path('tests/ble_s2_sim.c'))
pure=compile(Path('tests/ble_s2_test.c'))
def run(name,objects):
    exe=out/name
    subprocess.run([cc,'-no-pie','-fsanitize=address,undefined','-Wl,--gc-sections',*map(str,objects),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
run('logic',[*base,sim,pure])
run('connection-updates',[*base,sim,compile(Path('tests/ble_conn_update_test.c'))])
vendor=[compile(p,nb.flags()) for p in nb.sources()]
port=[compile(Path('open_ble')/(n+'.c'),nb.flags(vendor=False)) for n in ('ble_npl','ble_host')]
integration=compile(Path('tests/ble_s2_integration.c'))
run('integration',[*base,sim,*vendor,*port,integration])
