#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Drew Robinson
# Fetch everything the build needs into the repository, pinned: nothing is installed in $HOME, and nothing runs that
# isn't in tools/toolchain.lock. Every download is checked against the lock (SHA-256 for files, the commit for git)
# before it is unpacked; a mismatch stops the script. tools/get_toolchain.sh remains the quick way to the JieLi
# toolchain alone. After ESTAMPIE's tools/setup_toolchain.sh (Drew Robinson), offered to fm1-base under MPL-2.0.
#   .toolchain/jieli       JieLi's Linux toolchain (clang 4.0.1 for pi32v2); tools/build.py finds it here when
#                          JIELI_TOOLCHAIN is unset
#   .toolchain/ac79-sdk    the three files of the JieLi AC79 SDK that go into every package (a sparse checkout;
#                          Bluetooth and WiFi builds still need a whole checkout at $AC79_SDK)
#   .toolchain/node        Node, the binary only (no npm, npx or corepack), for the web tests
#   .toolchain/bin         osv-scanner (tools/osv_scan.sh)
#   .toolchain/uv          uv, where none is installed (a fresh CI runner): it makes .venv
#   .venv                  Python with the packages in uv.lock (uv sync --frozen; pyproject.toml says why these)
#   .toolchain/fm1-reader  fm1-reader at its pinned commit, when there is no checkout next to this repository (the
#                          emulator page's screen reader: tools/build.py --web)
# A step is skipped when its folder already holds the pinned version; change a pin and run it again to update.
# --only takes a comma-separated list of steps (jieli, ac79-sdk, node, python, osv-scanner, fm1-reader, uv): CI fetches
# just what a job needs through the same checks.
#   tools/setup_toolchain.sh [--only STEP,...]
set -eu
STEPS=jieli,ac79-sdk,node,python,osv-scanner,fm1-reader
USAGE="usage: tools/setup_toolchain.sh [--only STEP,...]"
while [ $# -gt 0 ]; do
    case "$1" in
        --only) [ $# -ge 2 ] || { echo "$USAGE"; exit 2; }; STEPS=$2; shift ;;
        *) echo "$USAGE"; exit 2 ;;
    esac
    shift
done
want() { case ",$STEPS," in *",$1,"*) true ;; *) false ;; esac; }
cd "$(dirname "$0")/.."
ROOT=$PWD
TC=.toolchain
DL=$TC/downloads
mkdir -p "$DL"

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) PLATFORM=darwin-arm64 ;;
    Linux-x86_64) PLATFORM=linux-x64 ;;
    *) echo "setup_toolchain: no pins for $(uname -s) $(uname -m) (tools/toolchain.lock)"; exit 1 ;;
esac

die() { echo "setup_toolchain: $*" >&2; exit 1; }

# The lock line for NAME on this platform ("any" and "git" lines match every platform): version, hash, url.
pin() {
    awk -v n="$1" -v p="$PLATFORM" '$1 == n && ($2 == p || $2 == "any" || $2 == "git") \
        { print $3, $4, $5; found = 1; exit } END { if (!found) exit 1 }' tools/toolchain.lock \
        || die "no $1 for $PLATFORM in tools/toolchain.lock"
}

sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

# Download URL to $DL once, over HTTPS only, and check it against the pinned hash (also when cached). Prints the
# file's path; callers stop when it fails (file=$(fetch ...) || exit 1).
fetch() {
    url=$1 want=$2 file=$DL/$(basename "$1")
    if [ ! -f "$file" ]; then
        curl --proto '=https' --tlsv1.2 -fL --retry 3 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    got=$(sha256 "$file")
    [ "$got" = "$want" ] || { rm -f "$file"; die "$(basename "$file"): SHA-256 $got, pinned $want (removed; not unpacked)"; }
    echo "$file"
}

# A folder is current when its .pin holds the lock's hash.
current() { [ -f "$1/.pin" ] && [ "$(cat "$1/.pin")" = "$2" ]; }

# Unpack a verified archive into a fresh folder, then swap it in for DEST.
unpack() {
    archive=$1 dest=$2 hash=$3
    rm -rf "$dest.new" && mkdir -p "$dest.new"
    tar -xf "$archive" -C "$dest.new"
    echo "$hash" > "$dest.new/.pin"
    rm -rf "$dest" && mv "$dest.new" "$dest"
}

# A git checkout at exactly the pinned commit (fetched by REF, a tag or branch, then checked). Any further arguments
# are the only paths checked out, and only their contents are fetched (sparse, blob-less): the rest of the repository
# never lands on disk. An older, whole checkout is cloned again.
checkout() {
    dest=$1 ref=$2 commit=$3 url=$4
    shift 4
    if [ ! -d "$dest/.git" ] || [ "$(git -C "$dest" rev-parse HEAD)" != "$commit" ] ||
        { [ $# -gt 0 ] && [ "$(git -C "$dest" sparse-checkout list 2>/dev/null)" != "$(printf '%s\n' "$@")" ]; } ||
        { [ $# -gt 0 ] && [ "$(git -C "$dest" config --get remote.origin.partialclonefilter)" != blob:none ]; }; then
        rm -rf "$dest"
        if [ $# -eq 0 ]; then
            git clone --quiet --no-recurse-submodules --depth 50 --no-checkout --branch "$ref" "$url" "$dest"
        else
            git clone --quiet --no-recurse-submodules --depth 50 --no-checkout --filter=blob:none --branch "$ref" \
                "$url" "$dest"
            git -C "$dest" sparse-checkout set --no-cone "$@"
        fi
        git -C "$dest" -c advice.detachedHead=false checkout --quiet "$commit" 2>/dev/null \
            || { rm -rf "$dest"; die "$url: $ref doesn't reach the pinned commit $commit"; }
    fi
    [ "$(git -C "$dest" rev-parse HEAD)" = "$commit" ] || die "$dest: not at $commit"
}

# JieLi toolchain.
if want jieli; then
    set -- $(pin jieli)
    if ! current $TC/jieli "$2"; then
        file=$(fetch "$3" "$2") || exit 1
        unpack "$file" $TC/jieli "$2"
        ln -sfn "jieli-linux-toolchains-$1" $TC/jieli/toolchain
    fi
    [ -x $TC/jieli/toolchain/pi32v2/bin/clang ] || die "$TC/jieli/toolchain: no pi32v2/bin/clang"
fi

# AC79 SDK.
if want ac79-sdk; then
    set -- $(pin ac79-sdk)
    # Only the three files a package takes (tools/build.py SDK_SHA256). The SDK's Windows tools (its JTAG debug server,
    # make, isd_download ..) are never used here, and antivirus flags some of them as potentially unwanted.
    checkout $TC/ac79-sdk "$1" "$2" "$3" /cpu/wl82/tools/uboot.boot /cpu/wl82/tools/cfg_tool.bin \
        /cpu/wl82/tools/cfg/eq_cfg_hw.bin
fi

# Node: the web page and browser emulator tests run on it. npm, npx and corepack are deleted: nothing here installs
# packages with them, and npm's own bundled dependencies are where most of a Node release's known vulnerabilities are.
if want node; then
    set -- $(pin node)
    if ! current $TC/node "$2"; then
        file=$(fetch "$3" "$2") || exit 1
        unpack "$file" $TC/node "$2"
        inner=$(ls -d $TC/node/node-v*)
        for d in "$inner"/*; do mv "$d" $TC/node/; done
        rmdir "$inner"
        rm -rf $TC/node/lib/node_modules $TC/node/bin/npm $TC/node/bin/npx $TC/node/bin/corepack $TC/node/include
    fi
    $TC/node/bin/node --version >/dev/null || die "$TC/node: node doesn't run"
fi

# uv, the pinned release (only asked for where none is installed: CI; --only uv). Its archive holds uv and uvx in one
# folder.
if want uv; then
    set -- $(pin uv)
    if ! current $TC/uv "$2"; then
        file=$(fetch "$3" "$2") || exit 1
        unpack "$file" $TC/uv "$2"
        for d in $TC/uv/uv-*/*; do mv "$d" $TC/uv/; done
        rmdir $TC/uv/uv-*
    fi
    [ "$($TC/uv/uv --version | cut -d' ' -f2)" = "$1" ] || die "$TC/uv: uv doesn't run, or isn't $1"
fi

# Python and the packages in uv.lock (hash-checked; pyproject.toml says why these versions): .venv.
if want python; then
    UV=$TC/uv/uv
    [ -x $UV ] || UV=$(command -v uv) || die "needs uv (https://docs.astral.sh/uv/, or --only uv)"
    "$UV" sync --frozen ${UV_SYNC_ARGS:-}              # (UV_SYNC_ARGS: other groups, as CI wants)
fi

# osv-scanner.
if want osv-scanner; then
    set -- $(pin osv-scanner)
    if [ "$(cat $TC/bin/osv-scanner.pin 2>/dev/null)" != "$2" ]; then
        mkdir -p $TC/bin
        file=$(fetch "$3" "$2") || exit 1
        cp "$file" $TC/bin/osv-scanner
        chmod 755 $TC/bin/osv-scanner
        echo "$2" > $TC/bin/osv-scanner.pin
    fi
fi

# fm1-reader at its pinned commit (MIT; its voice GPL-3.0-or-later), when no checkout sits next to this repo.
if want fm1-reader && [ ! -f ../fm1-reader/web/words.js ]; then
    set -- $(pin fm1-reader)
    checkout $TC/fm1-reader "$1" "$2" "$3"
fi

echo "setup_toolchain: done in $ROOT (tools/build.py APP_DIR, tests/run_tests.sh, tools/osv_scan.sh)"
