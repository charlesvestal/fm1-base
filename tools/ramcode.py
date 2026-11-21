# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Henri David
"""The check on an app's own RAM code (section .app_ram: plat.h PLAT_RAM_CODE), over the image's disassembly.

.ram_text is code that runs while the flash (XIP) is off: the flash driver. It calls nothing (tools/build.py check):
a call could land in XIP. .app_ram is the app's code that runs from RAM for speed, out of the XIP cache's way (an
audio path), with the flash on; it may call, but only code that is in RAM too, so no XIP instruction runs under it:

  - a call or a jump (a tail call) from it goes to RAM code: .app_ram, or the rest of .ram_text, whose own calls are
    followed in turn (the RAM code it reaches calls only RAM code, to any depth);
  - a call through a pointer ('call rN', 'goto rN') is one the build cannot follow: refused, with the XIP functions
    whose addresses that function loads (the likely targets: a pointer to XIP code is a call into XIP);
  - fm1.toml ram_code_allow = ["fn", ...] names the RAM functions whose calls through a pointer the app vouches for
    (FAR to a cold path in XIP, a pointer that only ever holds RAM code): each is printed with every build, and a
    name that matches no function in RAM is an error (the list stays what the code is). "fn?" names one an app's
    switch may leave out, or a build may place in XIP (its RAM placement switched off): absent or in XIP it is said in a
    note (XIP code makes no call out of RAM to vouch for); a plain "fn" must be RAM code;
  - a direct call between the app's RAM code and XIP, either way, is refused, ram_code_allow or not: RAM and the
    flash are ~2 MiB apart, the edge of a direct call's reach (R_PI32V2_LONG_JUMP_23M2, +-2 MiB), so one that links
    today fails at the link once the image grows or the RAM code moves (SEQ_RAM 2, 2026-11-02: trk_note_on ->
    drum_voice, 2,132,756 B). Such a call goes through a pointer (FAR, an app's FAST_CALL), or the callee is RAM code.

Constants are not checked: .app_ram code may read .rodata in XIP, so it does not run with the flash off (nothing
but the flash driver does: its windows hold the interrupts off).

The disassembly is objdump's (pi32v2): a function starts at a line 'name:', an instruction is 'ADDR: BYTES \\tTEXT'.
A direct call or jump is 'call N <sym : ADDR >' / 'goto N <...>' (also 'if (...) goto N <...>'), or, in the
conditional form whose label objdump prints on the next line, a bare 'call N' (the target is then the
instruction's address + N + its length).
"""
import bisect
import re

LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")
LABEL = re.compile(r"^([A-Za-z_$][\w.$]*):\s*$")
DIRECT = re.compile(r"\b(?:call|goto) (-?\d+)(?: <[^>]*: ([0-9a-f]+) >)?")
INDIRECT = re.compile(r"\b(?:call|goto) r\d+\b")
RAW = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$")
TB = re.compile(r"^tb[bh] \[r\d+\]")
ADDR_LOAD = re.compile(r"= -?\d+ <([A-Za-z_$][\w.$]*) : ([0-9a-f]+) >")


def symbol(syms, name):
    """an `objdump -t` symbol's value, None if absent"""
    m = re.search(r"^([0-9a-f]+) .*\s" + re.escape(name) + r"$", syms, re.M)
    return int(m.group(1), 16) if m else None


def functions(dis):
    """-> (starts, names, insns): the functions' start addresses (sorted), their names, and each one's instructions
    as (pc, length, text). A switch's 'tbh [rN]' / 'tbb [rN]' table after its instruction is data, not code (objdump
    decodes its entries as instructions: an entry 0x00c5 reads as 'call r5'): left out, up to its nearest target (each
    entry the halfwords from the table to its case), as tools/stack_depth.py reads it"""
    found, cur = {}, None
    pending, tab = None, None
    for ln in dis.splitlines():
        m = LABEL.match(ln.strip())
        if m:
            pending, tab = m.group(1), None
            continue
        mm = LINE.match(ln) or (RAW.match(ln) if tab is not None else None)   # (a table's '<unknown instruction>')
        if not mm:
            continue
        pc, raw, text = int(mm.group(1), 16), bytes(int(x, 16) for x in mm.group(2).split()), mm.group(3)
        size = len(raw)
        if tab is not None and tab["start"] <= pc < tab["end"]:
            tab["bytes"] += raw
            w, bs = tab["unit"], tab["bytes"]
            while tab["k"] + w <= len(bs) and tab["start"] + tab["k"] < tab["end"]:
                e = int.from_bytes(bs[tab["k"]:tab["k"] + w], "little")
                if e:
                    tab["end"] = min(tab["end"], tab["start"] + 2 * e)
                tab["k"] += w
            continue
        tab = None
        if pending is not None:
            cur = found.setdefault(pc, (pending, []))
            pending = None
        if cur is not None:
            cur[1].append((pc, size, text))
        if TB.match(text.strip()):
            st = pc + size
            tab = {"start": st, "end": st + 0x20000, "unit": 2 if text.strip().startswith("tbh") else 1, "k": 0,
                   "bytes": b""}
    starts = sorted(found)
    return starts, [found[a][0] for a in starts], [found[a][1] for a in starts]


def into_app_ram(starts, names, edges, ar, in_ram, code_starts):
    """-> errors for the direct calls from XIP code into the app's RAM code (.app_ram): as far out of reach as the
    other way (fm1-base's own XIP calls into the rest of .ram_text, the flash driver, are fm1-base's: not these)"""
    out = {}
    for i, a in enumerate(starts):
        if in_ram(a) or names[i].startswith(".GJTI"):
            continue
        for pc, t, _ in edges(i):
            if t is not None and ar[0] <= t < ar[1] and t in code_starts:
                out.setdefault((i, t), []).append(pc)
    by_start = {a: n for a, n in zip(starts, names)}
    return [f"XIP code calls RAM code directly: {names[i]} at {', '.join(f'{pc:#x}' for pc in pcs[:3])} -> "
            f"{by_start[t]} ({t:#x}): out of a direct call's reach as the image grows (FAR / FAST_CALL)"
            for (i, t), pcs in sorted(out.items())]


def check(syms, dis, allow=()):
    """-> (errors, notes) for the code between _ar_start and _ar_end (none there: nothing to say)"""
    ar = symbol(syms, "_ar_start"), symbol(syms, "_ar_end")
    rt = symbol(syms, "_rt_start"), symbol(syms, "_rt_end")
    if None in ar or None in rt or ar[1] <= ar[0]:
        return [], []
    starts, names, insns = functions(dis)
    by_name = {n: i for i, n in enumerate(names)}
    code_starts = {a for a, n in zip(starts, names) if not n.startswith(".GJTI")}   # (where a function begins)
    optional = {n[:-1] for n in allow if n.endswith("?")}
    absent = sorted(n for n in optional if n not in by_name)       # ("fn?": not built here, the switch off)
    allow = [n.rstrip("?") for n in allow if n.rstrip("?") not in absent]

    def in_ram(a):
        return rt[0] <= a < rt[1]

    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        return i if i >= 0 else None

    def edges(i):
        """a function's calls and jumps that leave it: [(pc, target or None for a pointer, text)]"""
        lo, hi = starts[i], (starts[i + 1] if i + 1 < len(starts) else 1 << 32)
        out = []
        for pc, size, text in insns[i]:
            if INDIRECT.search(text):
                out.append((pc, None, text))
                continue
            m = DIRECT.search(text)
            if m:
                t = int(m.group(2), 16) if m.group(2) else (pc + int(m.group(1)) + size) & 0xFFFFFFFF
                if not lo <= t < hi:
                    out.append((pc, t, text))
        return out

    def xip_fns_loaded(i):
        return sorted({n for _, _, text in insns[i] for n, a in ADDR_LOAD.findall(text)
                       if not in_ram(int(a, 16)) and n in by_name and starts[by_name[n]] == int(a, 16)})

    def at(pcs):
        return ", ".join(f"{pc:#x}" for pc in pcs[:3]) + (f" (+{len(pcs) - 3} more)" if len(pcs) > 3 else "")

    errors, used, direct = [], set(), {}
    roots = [i for i, a in enumerate(starts) if ar[0] <= a < ar[1]]
    seen, path_of = set(roots), {i: [names[i]] for i in roots}
    todo = list(roots)
    while todo:
        i = todo.pop()
        path = path_of[i]
        ptr, xip = [], {}                     # this function's calls through a pointer; into XIP, by target
        for pc, t, text in edges(i):
            if names[i] in allow and t is None:
                used.add(names[i])
            elif names[i] in allow and not in_ram(t):
                if t in code_starts:      # (vouched for or not: a direct call out of RAM is at the edge of its reach)
                    direct.setdefault((i, t), []).append(pc)
            elif t is None:
                ptr.append(pc)
            elif not in_ram(t):
                if t in code_starts:      # (a jump table's data word can read as a call to anywhere: no code, no call)
                    xip.setdefault(t, []).append(pc)
            else:
                j = owner(t)
                if j is not None and j not in seen:
                    seen.add(j)
                    path_of[j] = path + [names[j]]
                    todo.append(j)
        for t, pcs in sorted(xip.items()):
            j = owner(t)
            errors.append(f"RAM code calls XIP code: {' > '.join(path)} at {at(pcs)} -> "
                          f"{names[j] if j is not None else '?'} ({t:#x})")
        if ptr:
            xf = xip_fns_loaded(i)
            errors.append(f"RAM code calls through a pointer: {' > '.join(path)} at {at(ptr)}"
                          + (f"; it takes the address of XIP code {', '.join(xf)}" if xf else "; the target is not known")
                          + " (fm1.toml ram_code_allow if it is RAM code)")
    for (i, t), pcs in sorted(direct.items()):
        j = owner(t)
        errors.append(f"RAM code calls XIP code directly: {' > '.join(path_of[i])} at {at(pcs)} -> "
                      f"{names[j] if j is not None else '?'} ({t:#x}): ram_code_allow vouches for calls through a "
                      f"pointer only (a direct call is at the edge of its reach: FAR / FAST_CALL, or the callee in RAM)")
    errors += into_app_ram(starts, names, edges, ar, in_ram, code_starts)
    in_xip = []
    for n in allow:
        if n not in by_name or not in_ram(starts[by_name[n]]):
            if n in optional and n in by_name:
                in_xip.append(n)                    # ("fn?" built in XIP here: nothing out of RAM to vouch for)
            else:
                errors.append(f"fm1.toml ram_code_allow: {n} is no function in RAM code")
    allowed = f"; calls out of RAM or through a pointer allowed in {', '.join(sorted(used))} (ram_code_allow)" \
        if used else ""
    notes = [f"app RAM code (.app_ram): {ar[1] - ar[0]} B, {len(roots)} functions, {len(seen)} with the RAM code they "
             f"call (.ram_text {rt[1] - rt[0]} B in all){allowed}"]
    if absent:
        notes.append(f"ram_code_allow: not in this build: {', '.join(absent)}")
    if in_xip:
        notes.append(f"ram_code_allow: in XIP in this build (not RAM code): {', '.join(sorted(in_xip))}")
    return errors, notes
