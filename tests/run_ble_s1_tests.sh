#!/bin/sh
# SPDX-License-Identifier: MIT OR MPL-2.0
# Copyright (c) 2026 Julius Vitkauskas
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/host
"${CC:-cc}" -std=c11 -DBLE_HOST_TEST=1 -O1 -g -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    tests/ble_s1_test.c open_ble/ble_logic.c open_ble/ble_controller.c open_ble/ble_link_logic.c open_ble/ble_link.c open_ble/ble_timing.c \
    -o build/host/ble_s1_test
build/host/ble_s1_test

"${CC:-cc}" -std=c11 -DBLE_HOST_TEST=1 -DFELUCCA_BLE_TX_DBM=-20 -O1 -g -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    tests/ble_s1_test.c open_ble/ble_logic.c open_ble/ble_controller.c open_ble/ble_link_logic.c open_ble/ble_link.c open_ble/ble_timing.c \
    -o build/host/ble_s1_measured_power_test
build/host/ble_s1_measured_power_test
