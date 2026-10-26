#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Charles Vestal
# fm1-base's tests: the platform's pieces on the host (encoder, TRS jack, storage, fastmath, backup and restore, the
# screen mirror), the tools and web pages for them, the installer against a simulated FM-1, the update path against
# the template's package (tools/build.py template first), and the template in the simulator (tests/basic.txt).
#   tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
PY="${PYTHON:-python3}"
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"   # the package's SDK files (tools/fm1pkg_make.py)
OUT=template/build/tests
FAIL=0
mkdir -p "$OUT"
run() {
    name="$1"; shift
    if "$@" > "$OUT/$name.log" 2>&1; then
        echo "  ok   $name"
    else
        echo "  FAIL $name (see $OUT/$name.log)"
        tail -15 "$OUT/$name.log" | sed 's/^/       /'
        FAIL=1
    fi
}
run encoder sh -c "$CC -O2 -w -Ibsp/chip -Ibsp/boards/fm1 -o $OUT/encoder_test tests/encoder_test.c && $OUT/encoder_test"
run trs sh -c "$CC -O2 -w -Ibsp/chip -Ibsp/boards/fm1 -Idevice -o $OUT/trs_test tests/trs_test.c && $OUT/trs_test"
run storage sh -c "$CC -O2 -w -o $OUT/storage_test tests/storage_test.c && $OUT/storage_test"
run fastmath sh -c "$CC -O2 -w -o $OUT/fastmath_test tests/fastmath_test.c -lm && $OUT/fastmath_test"
run installer "$PY" tests/install_test.py
# the web installer's m-bootloader step (web/fm1ota.js installBoot) against a simulated FM-1, and finding the FM-1 by
# its port (unusual names, the loader under a new name, M-VAVE's Open firmware, resume's check, package checksums)
if command -v node >/dev/null 2>&1; then
    run installer-boot node tests/installer_boot_test.mjs web/m-bootloader/fm1-uboot-1.2.fwsc
    run installer-ports node tests/installer_ports_test.mjs web/m-bootloader/fm1-uboot-1.2.fwsc
else
    echo "  skip installer-boot (no node)"
fi
# backup and restore over USB (device/backup.c) on storage.c over a simulated flash, for a test app: the stream's
# frames, a restore refused or failed at every step changing nothing, a power cut at each write: the old set or the
# new, whole. Then tools/fm1_backup.py against a simulated FM-1 on that image, and the web page (web/backup) against
# the tool's own conversation and a simulated FM-1 in JS
run backup sh -c "$CC -std=c99 -Wall -Wextra -Werror -Wno-unused-function -O2 -o $OUT/backup_test tests/backup_test.c && \
    $OUT/backup_test $OUT/backup.bin"
run backup-tool "$PY" tests/backup_tool_test.py $OUT/backup.bin
if command -v node >/dev/null 2>&1; then
    run backup-page sh -c "$PY tests/backup_record.py $OUT/backup.bin $OUT/backup_transcript.json && \
        node tests/backup_page_test.mjs $OUT/backup_transcript.json"
else
    echo "  skip backup-page (no node)"
fi
PKG=template/build/template.fwsc
if [ -f "$PKG" ]; then
    run ota-entry sh -c "$CC -w -o $OUT/ota_test tests/ota_test.c && $OUT/ota_test $PKG web/m-bootloader/fm1-uboot-1.2.fwsc"
    run update-loader sh -c "head -c 60000 template/build/template.bin > $OUT/old_app.bin && \
        $PY tools/fm1pkg_make.py $OUT/old_app.bin template/build/loader/ota.bin $OUT/old.fwsc >/dev/null && \
        $CC -w -o $OUT/ldr_test tests/ldr_test.c && $OUT/ldr_test $OUT/old.fwsc $PKG"
else
    echo "  skip update path (no $PKG: tools/build.py template)"
fi
run host-build "$PY" tools/build.py template --host
# the screen mirrored over USB (device/mirror.c; the fonts: the template's build/gen): its pixel code on drawn bands,
# rows not sent twice, drops and redraws, the input reports, off, the remote; then the viewer's own decoder
# (web/mirror) on a session's stream against the device's screen, and tools/fm1_mirror.py making a PNG of it
run mirror sh -c "mkdir -p $OUT/mirror && $CC -std=c99 -Wall -Wextra -Werror -Wno-unused-function -O2 \
    -Itemplate/build/gen -o $OUT/mirror_test tests/mirror_test.c && $OUT/mirror_test --out $OUT/mirror"
if command -v node >/dev/null 2>&1; then
    run mirror-viewer node tests/mirror_test.mjs $OUT/mirror
else
    echo "  skip mirror-viewer (no node)"
fi
run mirror-png "$PY" tools/fm1_mirror.py $OUT/mirror.png --from $OUT/mirror/stream.syx
run template template/build/host/template_host template/tests/basic.txt template/build/host
[ $FAIL -eq 0 ] && echo "all tests passed" || echo "TESTS FAILED"
exit $FAIL
