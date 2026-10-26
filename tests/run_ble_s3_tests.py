#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR MPL-2.0
# Copyright (c) 2026 Julius Vitkauskas
import os,sys,subprocess
from pathlib import Path
os.environ['FELUCCA_BLE_STAGE']='3'
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import nimble_build as nb
os.chdir(nb.ROOT)
no_pairing='--no-pairing' in sys.argv
s31='--s31' in sys.argv
objects='--open-store' in sys.argv
release='--no-diag' in sys.argv
mic='--mic-stress' in sys.argv
reliability='--reliability' in sys.argv
flash_jobs='--flash-jobs' in sys.argv
assert not reliability or mic or (s31 and objects)
assert not flash_jobs or (s31 and objects and not mic and not reliability)
out=nb.ROOT/(('build/flash-job-tests'+('-no-pairing' if no_pairing else '-release' if release else '')) if flash_jobs else 'build/no-pairing-tests' if no_pairing else 'build/reliability-tests' if reliability and mic else 'build/reliability-store-tests' if reliability else 'build/mic-tests' if mic else 'build/release-tests' if release else 'build/open-tests' if objects else 'build/s31-tests' if s31 else 'build/s3-tests');out.mkdir(exist_ok=True,parents=True)
cc=os.environ.get('CC','cc')
common=['-DBLE_HOST_TEST=1','-DFELUCCA_BLE_STAGE=3','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-fno-builtin-memset','-fno-builtin-memcpy','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie']
common+=['-DFM1B_BLE_RELIABILITY='+('1' if reliability else '0')]
if no_pairing: common+=['-DFM1B_BLE_NO_PAIRING=1']
if release: common+=['-DFM1B_BLE_DIAG=0']
if s31: common+=['-DFELUCCA_BLE_S31=1']
if objects: common+=['-DFM1B_BLE_OPEN=1','-Wno-unused-function']
if mic:
    common+=['-DBLE_MIC_RADIO=1',f'-I{out}']
    # Compile the production S3 HAL RX/refill functions verbatim on a virtual
    # register bus, rather than a second implementation of their ordering.
    import re
    hal=(nb.ROOT/'open_ble/hal/fm1_ble.h').read_text()
    def extract(name):
        m=re.search(r'^(?:static )?(?:void|int) '+name+r'\([^;]+?\)\n\{',hal,re.M)
        assert m,name
        end=m.end(); depth=1
        while depth:
            if hal[end]=='{': depth+=1
            if hal[end]=='}': depth-=1
            end+=1
        return hal[m.start():end]+'\n'
    (out/'hal_mic_seam.inc').write_text(extract('bb_refill_limit')+
        'static void bb_refill(void) { bb_refill_limit(2); }\n'+extract('bb_connected_rx')+ ('\n#if FM1B_BLE_RELIABILITY\n'+extract('ble_port_diag_rx_read')+extract('ble_port_diag_rx_release')+'#endif\n' if reliability else ''))
    diag=(nb.ROOT/'device/ble_open_diag.c').read_text()
    (out/'mic_dump_seam.inc').write_text(diag[diag.index('static void open_ble_mic_dump(void)'):
        diag.index('\nstatic void open_ble_events_dump')])
def compile(p,flags=()):
    obj=out/(str(p).replace('/','_')+'.o')
    subprocess.run([cc,*common,*flags,'-c',str(p),'-o',str(obj)],check=True)
    return obj
def run(name,objects,args=(),extra_ldflags=()):
    exe=out/name
    subprocess.run([cc,'-no-pie','-fsanitize=address,undefined','-Wl,--gc-sections',
                    '-Wl,--wrap=tc_sha256_update,--wrap=tc_sha256_final,--wrap=memset,--wrap=memcpy,--wrap=ble_secure_zero',
                    *(['-Wl,--wrap=ble_ccm,--wrap=tc_aes_encrypt,--wrap=tc_aes128_set_encrypt_key'] if reliability and mic else []),*extra_ldflags,*map(str,objects),'-o',str(exe)],check=True)
    subprocess.run([str(exe),*args],check=True)
base=[compile(Path('open_ble')/(n+'.c'),nb.flags(vendor=False)) for n in ('ble_logic','ble_controller','ble_link_logic','ble_link','ble_midi','ble_heap','ble_crypto','ble_seed','ble_bonds','ble_timing','ble_privacy')]
if objects:
    base+=[compile(Path('open_ble/ble_store.c'),nb.flags(vendor=False))]
    # The R1-R4 store parser, kept verbatim as a fixture: the downgrade test runs it on the current format.
    names=('init','seed_load','seed_save','bond_load','bonds_save')
    base+=[compile(Path('tests/fixtures/open_ble/r4-store/ble_store.c'),[*nb.flags(vendor=False),
        *['-Dble_object_'+n+'=r4_ble_object_'+n for n in names]])]
if flash_jobs:
    # fm1-base's own flash-job glue, verbatim: the functions are cut from the device sources (unity.c, plat_fm1.c,
    # ble_open_platform.c) into includes; device/ble_open.c, storage.c, backup.c and bsp ota.c are included whole.
    import re
    def cut(path,*names):
        text=(nb.ROOT/path).read_text(); parts=[]
        for name in names:
            m=re.search(r'^(?:static )?(?:inline )?[a-z_0-9 ]+?\b'+name+r'\([^;{]*?\)[^;{]*?\{',text,re.M)
            assert m,(path,name)
            end=m.end(); depth=1
            while depth:
                if text[end]=='{': depth+=1
                if text[end]=='}': depth-=1
                end+=1
            parts.append(text[m.start():end]+'\n')
        return '\n'.join(parts)
    (out/'flash_store_seam.inc').write_text(cut('device/unity.c','st_erase','st_prog'))
    (out/'flash_ota_seam.inc').write_text(cut('device/unity.c','ota_in_area','ota_erase','ota_prog','ota_fread'))
    (out/'flash_backup_seam.inc').write_text(cut('device/unity.c','rs_begin','bu_flush','rs_rec_erase','rs_rec_prog','rs_rec_read'))
    (out/'flash_plat_seam.inc').write_text(cut('device/plat_fm1.c','plat_data_erase','plat_data_write','plat_store_load','plat_store_save')+
        cut('device/ble_open_platform.c','ble_port_object_load','ble_port_object_save'))
    macros=subprocess.check_output([cc,'-dM','-E','-DFM1B_BLE_OPEN=1','-Ibsp/chip','-include','stdint.h',
        '-include','open_ble/hal/fm1_ble_flash.h','-'],input=b'').decode()
    (out/'flash_ranges.inc').write_text('\n'.join(l for l in macros.splitlines() if l.startswith('#define FL_'))+'\n')
    # The main loop's two call sites (device/main_fm1.c), which the test repeats: the update's and the restore's.
    main=(nb.ROOT/'device/main_fm1.c').read_text()
    assert re.search(r'flash_quiet_begin\(FLASH_QUIET_UPDATE\);[^\n]*\n(?:[^\n]*\n){0,2}\s*ota_session\(\);[^\n]*\n\s*flash_quiet_end\(\);',main)
    assert re.search(r'rc = restore_rx\(len, crc, &begun\);\n\s*flash_quiet_end\(\);',main)
    sim=[compile(Path('tests/ble_s2_sim.c'),nb.flags(vendor=False)),
         compile(Path('tests/ble_s3_sim.c'),[*nb.flags(vendor=False),'-Dble_port_storage_idle=ble_s3_sim_storage_idle']),
         compile(Path('open_ble/ble_health.c'),nb.flags(vendor=False))]
else:
    sim=[compile(Path('tests')/(n+'.c'),nb.flags(vendor=False)) for n in ('ble_s2_sim','ble_s3_sim')]
if objects and not flash_jobs: sim+=[compile(Path('tests/ble_open_sim_store.c'),nb.flags(vendor=False))]
vendor=[compile(p,[*nb.flags(),"-Wno-old-style-declaration",*(['-fno-sanitize=shift-base'] if p.name=='aes_encrypt.c' else [])]) for p in nb.sources()]
port=[compile(Path('open_ble')/(n+'.c'),nb.flags(vendor=False)) for n in ('ble_npl','ble_host')]
if flash_jobs:
    run('flash-jobs',[*base,*sim,*vendor,*port,compile(Path('tests/ble_open_flash_jobs_test.c'),[*nb.flags(vendor=False),f'-I{out}'])])
    sys.exit(0)
if no_pairing:
    run('no-pairing',[*base,*sim,*vendor,*port,compile(Path('tests/ble_s2_integration.c'),nb.flags(vendor=False))])
    sys.exit(0)
if mic:
    run('mic-stress',[*base,*sim,*vendor,*port,compile(Path('tests/ble_mic_stress.c'),nb.flags(vendor=False))],
        [os.environ.get('BLE_MIC_EVENTS','10000000')])
    subprocess.run([str(out/'mic-stress'),'--trace-timeout'],check=True)
    if reliability:
        for case in ('counter-2','counter-1','counter1','counter2','counter-zero','counter-max',
                     'direction','previous-key','previous-context','copy-byte','dma-byte',
                     'aes-transient','aes-persistent','key-byte','derive-block','tx-block','phase','background','tx-toggle','audio-preempt','old-command','kat-under-load'):
            subprocess.run([str(out/'mic-stress'),'--r7',case],check=True)
    sys.exit(0)
if '--capacity-only' in sys.argv:
    assert objects and s31
    run('pairing-f29', [*base,*sim,*vendor,*port,compile(Path('tests/ble_s3_integration.c'),nb.flags(vendor=False))],['f29'])
    sys.exit(0)
run('nimble110-crypto', [*base,*sim,*vendor,*port,compile(Path('tests/ble_nimble110_crypto.c'),nb.flags(vendor=False))])
run('nimble110-gap', [*base,*sim,*vendor,*port,compile(Path('tests/ble_nimble110_gap.c'),nb.flags(vendor=False))],
    extra_ldflags=['-Wl,--wrap=ble_hs_hci_cmd_tx'])
if not objects and not release: run('logic' ,[*base,*sim,*vendor,*port,compile(Path('tests/ble_s3_test.c'),nb.flags(vendor=False))])
run('s2-regression',[*base,*sim,*vendor,*port,compile(Path('tests/ble_s2_integration.c'),nb.flags(vendor=False))])
if not release: run('connection-updates', [*base,*sim,*vendor,*port,compile(Path('tests/ble_conn_update_test.c'),nb.flags(vendor=False))])
integration=compile(Path('tests/ble_s3_integration.c'),nb.flags(vendor=False))
if s31 and not objects and not release:
    run('privacy-store-timing', [*base,*sim,*vendor,*port,compile(Path('tests/ble_s31_test.c'),nb.flags(vendor=False))])
if s31 and not release:
    run('resumable-steps', [*base,*sim,*vendor,*port,compile(Path('tests/ble_s31_steps.c'),nb.flags(vendor=False))])
if objects: run('object-store', [*base,*sim,*vendor,*port,compile(Path('tests/ble_open_store_test.c'),nb.flags(vendor=False))])
for mode in ((('f29',) if objects and s31 else ())+('legacy','sc','rpa','repeat-legacy','repeat-sc') if s31 else ('legacy','sc')):
    run('pairing-'+mode,[*base,*sim,*vendor,*port,integration],[mode])
