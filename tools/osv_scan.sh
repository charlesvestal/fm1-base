#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Drew Robinson
# Check everything the build runs against the OSV vulnerability database (osv.dev), with osv-scanner. After ESTAMPIE's
# tools/osv_scan.sh, offered to fm1-base under MPL-2.0.
#   python       uv.lock (the font generator, the installer tools)
#   node         whether a newer release of the pinned Node line is a security release (OSV doesn't cover Node)
#   build-image  the pinned container the JieLi tools run in (needs Docker or podman); accepted findings, each with a reason and
#                an expiry, are in tools/osv-build-image.toml. A newer image of the tag is scanned too: once it fixes an
#                accepted finding and is two days old, the scan fails, so the pin moves before the expiry
#   espeak       the screen reader's voice (fm1-reader's web/espeak/, which tools/build.py --web puts in the emulator
#                page): eSpeak NG at the commit it was built from, asked of osv.dev by commit (osv-scanner scans no
#                prebuilt WebAssembly); accepted findings, one a line with an expiry and a reason, in
#                tools/osv-espeak-accepted.txt
# Parts whose tools aren't fetched are skipped. Reports (JSON) in build/osv/. Exits 1 on any finding.
#   tools/osv_scan.sh [part ...]       (default: every part)
set -u
cd "$(dirname "$0")/.."
OSV=.toolchain/bin/osv-scanner
[ -x "$OSV" ] || OSV=$(command -v osv-scanner) || { echo "osv_scan: no osv-scanner (tools/setup_toolchain.sh)"; exit 1; }
mkdir -p build/osv
PARTS=${*:-python node build-image espeak}
fail=0

# Scan with the given arguments; the report goes to build/osv/NAME.json, a summary to the terminal. A scan that sees
# no packages at all fails too: it would otherwise pass without checking anything.
scan() {
    name=$1; shift
    "$OSV" scan "$@" --format json --all-packages > "build/osv/$name.json" 2> "build/osv/$name.log"
    python3 -I - "build/osv/$name.json" "$name" <<'PY' || fail=1
import json, sys
path, name = sys.argv[1], sys.argv[2]
try:
    results = json.load(open(path)).get("results") or []
except ValueError:
    sys.exit(f"osv_scan: {name}: no report (build/osv/{name}.log)")
packages = [p for r in results for p in r["packages"]]
found = [(v["id"], p["package"]["name"], p["package"].get("version", "?")) for p in packages for v in p.get("vulnerabilities", [])]
if not packages:
    sys.exit(f"osv_scan: {name}: FAILED, no packages seen (build/osv/{name}.log)")
if found:
    print(f"osv_scan: {name}: {len(found)} FINDINGS in {len(packages)} packages (build/osv/{name}.json):")
    for f in sorted(set(found)):
        print("    %s  %s %s" % f)
    sys.exit(1)
print(f"osv_scan: {name}: {len(packages)} packages, no known vulnerabilities")
PY
}
skip() { echo "osv_scan: $1: skipped ($2)"; }

for part in $PARTS; do
    case $part in
    python)
        scan python source --lockfile uv.lock ;;
    node)
        # The Node pinned in the lock, not whatever node is on the PATH (a CI runner's own is not ours).
        have=$(awk '$1 == "node" { print "v" $3; exit }' tools/toolchain.lock)
        if [ -n "$have" ]; then
            newer=$(curl --proto '=https' -fsSL https://nodejs.org/dist/index.json | python3 -I -c '
import json, sys
have = sys.argv[1].lstrip("v").split(".")
rel = [r for r in json.load(sys.stdin) if r["version"].lstrip("v").split(".")[0] == have[0]]
newer = [r for r in rel if [int(x) for x in r["version"].lstrip("v").split(".")] > [int(x) for x in have]]
print(" ".join(r["version"] for r in newer if r["security"]))' "$have") || newer="?"
            case $newer in
                "") echo "osv_scan: node: $have, no newer security release in its line" ;;
                "?") echo "osv_scan: node: couldn't read nodejs.org/dist/index.json"; fail=1 ;;
                *) echo "osv_scan: node: $have; security releases since: $newer (update tools/toolchain.lock)"; fail=1 ;;
            esac
        else skip node "no node in tools/toolchain.lock"; fi ;;
    build-image)
        # Docker or podman, the first that answers: a docker installed with no daemon running must not hide a working
        # podman. save takes --platform only where the tool's save has it (podman's hasn't: its pull of one platform
        # keeps that one alone), asked of save itself, since a podman reached as docker gives its version as docker's.
        CTR=
        for c in docker podman; do
            if command -v $c >/dev/null && $c info >/dev/null 2>&1; then CTR=$c; break; fi
        done
        if [ -n "$CTR" ]; then
            PLAT=
            "$CTR" save --help 2>/dev/null | grep -q -e --platform && PLAT="--platform linux/amd64"
            IMG=$(awk '$1 == "build-image" { print $5 "@sha256:" $4 }' tools/toolchain.lock)
            rm -f build/osv/build-image*.json             # (a report left from the last run must not stand in)
            # Each step fails the scan on its own: a pull or save that fails must not leave the image unscanned.
            if ! "$CTR" pull -q --platform linux/amd64 "$IMG" >/dev/null; then
                echo "osv_scan: build-image: FAILED, couldn't pull $IMG"; fail=1
            elif ! "$CTR" save $PLAT -o build/osv/build-image.tar "$IMG"; then
                echo "osv_scan: build-image: FAILED, couldn't save $IMG for scanning"; fail=1
            else
                scan build-image image --config tools/osv-build-image.toml --archive build/osv/build-image.tar
                "$OSV" scan image --archive build/osv/build-image.tar --format json --all-packages \
                    > build/osv/build-image-all.json 2> /dev/null
            fi
            rm -f build/osv/build-image.tar
            # The tag's digest now, from the registry itself (no buildx, which podman hasn't got).
            latest=$(python3 -I - "${IMG%@*}" <<'PY'
import json, re, sys, urllib.request
host, repo, tag = re.match(r"([^/]+)/(.+):([^:/]+)$", sys.argv[1]).groups()
url = f"https://{host}/v2/{repo}/manifests/{tag}"
accept = ", ".join(["application/vnd.oci.image.index.v1+json", "application/vnd.docker.distribution.manifest.list.v2+json",
                    "application/vnd.oci.image.manifest.v1+json", "application/vnd.docker.distribution.manifest.v2+json"])
def head(headers):
    return urllib.request.urlopen(urllib.request.Request(url, method="HEAD", headers=headers), timeout=30)
try:
    r = head({"Accept": accept})
except urllib.error.HTTPError as e:                   # a registry that wants an anonymous token first
    m = e.code == 401 and re.match(r'Bearer realm="([^"]+)"(?:,service="([^"]+)")?', e.headers.get("WWW-Authenticate", ""))
    if not m:
        raise
    q = f"?scope=repository:{repo}:pull" + (f"&service={m.group(2)}" if m.group(2) else "")
    token = json.load(urllib.request.urlopen(m.group(1) + q, timeout=30))["token"]
    r = head({"Accept": accept, "Authorization": f"Bearer {token}"})
print(r.headers["Docker-Content-Digest"])
PY
            ) || latest=
            if [ -z "$latest" ]; then
                echo "osv_scan: build-image: FAILED, couldn't read ${IMG%@*}'s digest from the registry"; fail=1
            elif [ "${IMG#*@}" != "$latest" ]; then
                # A newer image: scan it too, without the accepted list, and fail once it fixes an accepted finding
                # and is two days old (its upload time from the registry; distroless dates its images 1970), so a
                # fix is taken soon, not at the accepted list's expiry. One that fixes none is only reported.
                NEW="${IMG%@*}@$latest"
                "$CTR" pull -q --platform linux/amd64 "$NEW" >/dev/null &&
                    "$CTR" save $PLAT -o build/osv/build-image-newer.tar "$NEW" &&
                    "$OSV" scan image --archive build/osv/build-image-newer.tar --format json --all-packages \
                        > build/osv/build-image-newer.json 2> /dev/null
                rm -f build/osv/build-image-newer.tar
                python3 -I - "$NEW" tools/osv-build-image.toml build/osv/build-image-all.json \
                    build/osv/build-image-newer.json <<'PY' || fail=1
import json, re, sys, time, urllib.request
new, accepted_file, cur_file, newer_file = sys.argv[1:]
def ids(path):
    try:
        results = json.load(open(path)).get("results") or []
    except (OSError, ValueError):
        return None
    return {v["id"] for r in results for p in r["packages"] for v in p.get("vulnerabilities", [])}
accepted = set(re.findall(r'^id = "([^"]+)"', open(accepted_file).read(), re.M))
cur, newer = ids(cur_file), ids(newer_file)
name, digest = new.split("@")
print(f"osv_scan: build-image: {name} has a newer digest, {digest} (update tools/toolchain.lock)")
if cur is None or newer is None:
    sys.exit("osv_scan: build-image: FAILED, the newer image could not be scanned (build/osv/build-image-newer.json)")
fixed = sorted(accepted & cur - newer)
if not fixed:
    print("osv_scan: build-image: the newer image fixes none of the accepted findings")
    sys.exit(0)
age = None
m = re.match(r"gcr\.io/(.+):", name)                 # (the registry's upload time: gcr.io's tag list has it)
if m:
    try:
        with urllib.request.urlopen(f"https://gcr.io/v2/{m.group(1)}/tags/list", timeout=30) as r:
            age = (time.time() - int(json.load(r)["manifest"][digest]["timeUploadedMs"]) / 1000) / 86400
    except (OSError, ValueError, KeyError):
        pass
print(f"osv_scan: build-image: the newer image fixes {len(fixed)} accepted: {' '.join(fixed)}")
if age is not None and age < 2:
    print(f"osv_scan: build-image: it is {age:.1f} days old; it fails this scan from 2 days")
    sys.exit(0)
sys.exit("osv_scan: build-image: FAILED, take the newer image (" + (f"{age:.0f} days old" if age is not None else
         "age unknown") + "), check the build is byte-identical, and remove the fixed ids from " + accepted_file)
PY
            fi
            python3 -I - tools/osv-build-image.toml build/osv/build-image-all.json <<'PY'
import json, re, sys
accepted = re.findall(r'^id = "([^"]+)"', open(sys.argv[1]).read(), re.M)
try:                                                  # accepted ids the pinned image no longer has: report only
    results = json.load(open(sys.argv[2])).get("results") or []
except (OSError, ValueError):
    sys.exit(0)
have = {v["id"] for r in results for p in r["packages"] for v in p.get("vulnerabilities", [])}
gone = [i for i in accepted if i not in have]
if have and gone:
    print(f"osv_scan: build-image: {len(gone)} accepted ids no longer found, can go from {sys.argv[1]}: {' '.join(gone)}")
PY
        else skip build-image "no Docker or podman running"; fi ;;
    espeak)
        R="${FM1_READER:-../fm1-reader}"               # (no checkout next to it: the pinned one, setup_toolchain.sh)
        [ -n "${FM1_READER:-}" ] || [ -f "$R/web/words.js" ] || R=.toolchain/fm1-reader
        C=$(sed -n 's/.*espeak-ng commit `\([0-9a-f]\{40\}\)`.*/\1/p' "$R/web/espeak/README.md" 2>/dev/null)
        if [ -n "$C" ]; then
            if ! curl --proto '=https' -fsS -X POST https://api.osv.dev/v1/query -d "{\"commit\":\"$C\"}" > build/osv/espeak.json; then
                echo "osv_scan: espeak: FAILED, couldn't ask osv.dev"; fail=1
            else python3 -I - build/osv/espeak.json tools/osv-espeak-accepted.txt "$C" <<'PY' || fail=1
import json, sys
import datetime
found = [v["id"] for v in json.load(open(sys.argv[1])).get("vulns", [])]
try:                                                  # id, expiry (YYYY-MM-DD), reason: an expired one counts again
    rows = [l.split(None, 2) for l in open(sys.argv[2]) if l.strip() and not l.startswith("#")]
except OSError:
    rows = []
today = datetime.date.today().isoformat()
ok = {r[0] for r in rows if len(r) == 3 and r[1] >= today}
expired = sorted(r[0] for r in rows if len(r) == 3 and r[1] < today and r[0] in found)
if expired:
    print(f"osv_scan: espeak: accepted until a date now past: {' '.join(expired)} (re-check them in {sys.argv[2]})")
new = sorted(set(found) - ok)
if new:
    print(f"osv_scan: espeak: espeak-ng {sys.argv[3][:12]}: {len(new)} FINDINGS not accepted (build/osv/espeak.json): {' '.join(new)}")
    sys.exit(1)
print(f"osv_scan: espeak: espeak-ng {sys.argv[3][:12]}: {len(found)} known, all accepted in {sys.argv[2]}")
PY
            fi
        else skip espeak "no fm1-reader (FM1_READER)"; fi ;;
    *) echo "osv_scan: no part $part"; exit 2 ;;
    esac
done
[ $fail = 0 ] && echo "osv_scan: clean" || echo "osv_scan: FAILED"
exit $fail
