#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0
# Copyright (C) 2026 Henri David
# The own Wi-Fi stack's host tests (wifi/, device/wifi_own.c: wifi_stack = "own"; Optimist's), and Ableton Link's
# (link/: GPL-3.0-only, link = true), built with UBSan (and ASan where its runtime runs). Then the Link example in the
# host simulator over its simulated network (host/wifi_air.c: an access point, DHCP, a Link peer; nothing of this
# computer's network), and MODE AP on it (tests/wifi/link_ap: a client of the FM-1's own access point). Run from tests/run_tests.sh, or alone:  sh tests/wifi/run.sh
set -u
cd "$(dirname "$0")/../.."
CC="${CC:-cc}"
PY="${PYTHON:-python3}"
OUT="${OUT:-template/build/tests/wifi}"
FAIL=0
mkdir -p "$OUT"
echo 'int main(void) { return 0; }' > "$OUT/san_probe.c"
san_runs() {   # the sanitizers in $1 build and run (macOS 27's ASan runtime hangs before main: then UBSan alone)
    $CC $1 -o "$OUT/san_probe" "$OUT/san_probe.c" 2>/dev/null || return 1
    "$OUT/san_probe" & p=$!
    i=0
    while kill -0 $p 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    kill -0 $p 2>/dev/null && { kill $p; wait $p 2>/dev/null; return 1; }
    wait $p
}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
san_runs "$SAN" || SAN="-fsanitize=undefined -fno-sanitize-recover=all"
san_runs "$SAN" || SAN=""
T=tests/wifi
INC="-Ihost/wifi_fake -Ibsp/chip -Iwifi -Iinclude"
t() {   # t NAME SOURCE [FLAGS...]: build SOURCE with FLAGS as NAME and run it
    name="$1"; src="$2"; shift 2
    if $CC -std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-function $SAN $INC "$@" -o "$OUT/$name" "$T/$src" -lm \
        > "$OUT/$name.log" 2>&1 && "$OUT/$name" >> "$OUT/$name.log" 2>&1; then
        echo "  ok   wifi/$name"
    else
        echo "  FAIL wifi/$name (see $OUT/$name.log)"
        tail -15 "$OUT/$name.log" | sed 's/^/       /'
        FAIL=1
    fi
}
t wifi_sta wifi_sta_test.c            # WPA2: SHA-1, HMAC, PBKDF2, AES, key wrap, PTK, the 4-way, key install, the IP layer
t wifi_ap wifi_ap_test.c              # the authenticator (the host's simulated access point) against the station
t wifi_apip wifi_apip_test.c          # the IP layer as an access point (the simulated one's): FromDS, ARP, DHCP, ping
t wifi_nets wifi_nets_test.c          # the networks kept, the scan's table, the beacons' security
t wifi_wlc wifi_wlc_test.c -Idevice   # the coexistence table (device/fm1_wifi_wlc.h) through felucca-bsp's fm1_wifi_up, word for word
t wifi_tune wifi_tune_test.c -DPLAT_HOST -DFM1B_WIFI=1 -DFM1B_WIFI_OWN=1 -DFM1B_WIFI_AP=1 -DFM1B_BLE=1   # the tuning: (a) the stored calibration, (b) the RX filter, (c) the AGC, (f) the rate
t wifi_apd wifi_apd_test.c -DPLAT_HOST -DFM1B_WIFI=1 -DFM1B_WIFI_OWN=1 -DFM1B_WIFI_AP=1 -DFM1B_BLE=1   # MODE AP (device/wifi_own_ap.c)
t wifi_apd_mem wifi_apd_test.c -DPLAT_HOST -DFM1B_WIFI=1 -DFM1B_WIFI_OWN=1 -DFM1B_WIFI_AP=1 -DFM1B_BLE=1 -DFM1B_WIFI_MEM=1   # .. its state lent by the app (wifi_mem = "app")
if "$PY" tests/wifi/remote_tune_test.py > "$OUT/remote_tune.log" 2>&1; then   # fm1_remote.py's 'wifi tune' letters
    echo "  ok   wifi/remote_tune"
else
    echo "  FAIL wifi/remote_tune (see $OUT/remote_tune.log)"
    FAIL=1
fi
t link link_test.c                    # Ableton Link (link/, GPL-3.0-only): wire format, join, tempo, phase, start / stop
t link_mem link_mem_test.c -DPLAT_HOST -DFM1B_WIFI=1 -DFM1B_WIFI_OWN=1 -DFM1B_LINK=1 -DFM1B_BLE=1 -DFM1B_WIFI_MEM=1 -Ilink   # Link's node lent by the app
# the Link example on its simulated network: join, DHCP, the session's tempo and phase, both ways, start / stop, the
# network lost and back, a reboot
if "$PY" tools/build.py examples/link --host > "$OUT/link_host_build.log" 2>&1 &&
    examples/link/build/host/link_host examples/link/tests/link.txt examples/link/build/host > "$OUT/link_host.log" 2>&1; then
    echo "  ok   wifi/link_host (examples/link/tests/link.txt)"
else
    echo "  FAIL wifi/link_host (see $OUT/link_host_build.log, $OUT/link_host.log)"
    grep FAIL "$OUT/link_host.log" | head -15 | sed 's/^/       /'
    FAIL=1
fi
# MODE AP on the simulated network: the Link example with wifi_ap = true (tests/wifi/link_ap), the simulator's client
# (host/wifi_air_client.c) joins the FM-1's access point, gets an address and is a Link peer over it
if "$PY" tools/build.py tests/wifi/link_ap --host > "$OUT/link_ap_build.log" 2>&1 &&
    tests/wifi/link_ap/build/host/linkap_host tests/wifi/link_ap/tests/ap.txt tests/wifi/link_ap/build/host > "$OUT/link_ap.log" 2>&1; then
    echo "  ok   wifi/link_ap (tests/wifi/link_ap/tests/ap.txt)"
else
    echo "  FAIL wifi/link_ap (see $OUT/link_ap_build.log, $OUT/link_ap.log)"
    grep FAIL "$OUT/link_ap.log" | head -15 | sed 's/^/       /'
    FAIL=1
fi
exit $FAIL
