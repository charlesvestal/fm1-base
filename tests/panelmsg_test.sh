#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Bitbazooka LLC (Les, les.audio)
# tests/panelmsg_test.c (panel messages, panelmsg.h and panelmsg.c, on fm1-base's own usb.c, ota.c, announce.c,
# panel.c and panelmsg_base.c): felucca-bsp's usb.c and ota.c are copied, byte for byte, beside nothing but themselves,
# so their "fm1_usb.h" is the register model (tests/panelmsg/fm1_usb.h) and not the chip's.
#   sh tests/panelmsg_test.sh
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp bsp/chip/usb.c bsp/chip/ota.c "$T/"
cmp bsp/chip/usb.c "$T/usb.c" && cmp bsp/chip/ota.c "$T/ota.c"
$CC -std=gnu11 -O1 -Wall -Wno-unused-function -I"$T" -Itests/panelmsg -Ibsp/chip -Ibsp/boards/fm1 -Idevice -Iinclude \
    ${PANELMSG_TEST_CFLAGS:-} -o "$T/panelmsg_test" tests/panelmsg_test.c
"$T/panelmsg_test"
