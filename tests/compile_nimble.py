#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR MPL-2.0
# Copyright (c) 2026 Julius Vitkauskas
import sys,subprocess
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import nimble_build as nb
out=nb.ROOT/'build/s2-host';out.mkdir(exist_ok=True,parents=True)
errors=[]
for i,p in enumerate(nb.sources()):
    cmd=['cc','-DBLE_HOST_TEST=1','-std=c11','-Os','-ffunction-sections','-fdata-sections','-Wall','-Wextra','-Werror','-Wno-unused-parameter',*nb.flags(),'-c',str(p),'-o',str(out/(p.stem+'.o'))]
    r=subprocess.run(cmd,capture_output=True,text=True)
    if r.returncode: errors.append((str(p.relative_to(nb.ROOT)),r.stderr))
for p,e in errors[:12]: print(p+'\n'+e)
print(f'NimBLE compiled {len(nb.sources())-len(errors)}/{len(nb.sources())} objects')
sys.exit(bool(errors))
