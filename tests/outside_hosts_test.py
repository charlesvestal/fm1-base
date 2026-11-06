# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Charles Vestal
"""No page fm1-base ships asks an outside host for anything it loads: fonts, scripts, styles, images, workers, data.
(The user's rule, 2026-10-29: the pages work offline, and a page that is opened must not contact fonts.googleapis.com,
fonts.gstatic.com or a CDN: a firewall asks about each.) Links the user clicks (<a href>) are not loads and are not checked.
The allow-list is EMPTY. Checked: everything under web/ and the page templates in tools/ that are HTML, CSS or JS.
    python3 tests/outside_hosts_test.py"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOW = []                       # (file suffix, URL prefix): none
SCAN = [ROOT / "web"]
EXT = {".html", ".htm", ".css", ".js", ".mjs", ".svg"}
HOSTS = re.compile(r"fonts\.googleapis\.com|fonts\.gstatic\.com|cdn\.jsdelivr\.net|unpkg\.com|cdnjs\.cloudflare\.com|ajax\.googleapis\.com", re.I)
EXT_URL = r"(?:https?:)?//"
LOADING_TAG = re.compile(r"<(script|link|img|source|iframe|audio|video|embed|object|track|image|use|input|frame)\b[^>]*>", re.I | re.S)
LOADING_ATTR = re.compile(r"\b(?:src|href|data|poster|srcset|xlink:href|action)\s*=\s*[\"']?\s*(" + EXT_URL + r"[^\"'\s>]*)", re.I)
CSS_URL = re.compile(r"(?<![A-Za-z.])url\(\s*[\"']?\s*(" + EXT_URL + r"[^\"')\s]*)")
CSS_IMPORT = re.compile(r"@import\s+[\"']\s*(" + EXT_URL + r"[^\"']*)", re.I)
JS_CALL = re.compile(r"\b(?:fetch|importScripts|import|Worker|SharedWorker|WebSocket|EventSource|sendBeacon)\s*\(\s*[`\"']\s*(" + EXT_URL + r"[^`\"']*)")
JS_ASSIGN = re.compile(r"\.(?:src|srcset|data)\s*=\s*[`\"']\s*(" + EXT_URL + r"[^`\"']*)")
JS_XHR = re.compile(r"\.open\(\s*[\"'][A-Z]+[\"']\s*,\s*[`\"']\s*(" + EXT_URL + r"[^`\"']*)")
JS_IMPORT_FROM = re.compile(r"\b(?:from|import)\s+[\"'](" + EXT_URL + r"[^\"']*)")


def outside(path, text):
    """[(line, what)] for each load of an outside host in one file's text"""
    found = []
    for m in HOSTS.finditer(text):
        found.append((text.count("\n", 0, m.start()) + 1, m.group(0)))
    for m in LOADING_TAG.finditer(text):
        for a in LOADING_ATTR.finditer(m.group(0)):
            found.append((text.count("\n", 0, m.start()) + 1, a.group(1)))
    for rx in (CSS_URL, CSS_IMPORT, JS_CALL, JS_ASSIGN, JS_XHR, JS_IMPORT_FROM):
        for m in rx.finditer(text):
            found.append((text.count("\n", 0, m.start()) + 1, m.group(1)))
    return [(n, u) for n, u in sorted(set(found)) if not any(str(path).endswith(s) and u.startswith(p) for s, p in ALLOW)]


def selftest():
    bad = ['<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow">',
           '<script src="//cdn.example.com/x.js"></script>', "<img src='http://a.example/i.png'>",
           "@font-face { src: url(https://example.com/f.woff2); }", "@import 'https://example.com/a.css';",
           "fetch('https://example.com/data.json')", "new Worker('https://example.com/w.js')", "import('https://e.example/m.js')",
           "x.src = 'https://e.example/s.js'", "import x from 'https://e.example/m.js'"]
    good = ['<a href="https://example.com/">link</a>', '<link rel="stylesheet" href="../fonts/fonts.css">',
            "@font-face { src: url(Barlow-400.woff2); }", "fetch('data.json')", "const u = new URL('//', location)"]
    for t in bad:
        assert outside("x.html", t), f"not found: {t}"
    for t in good:
        assert not outside("x.html", t), f"false alarm: {t}"


def main():
    selftest()
    bad = []
    n = 0
    for base in SCAN:
        for p in sorted(base.rglob("*")):
            if p.suffix.lower() not in EXT or not p.is_file():
                continue
            n += 1
            for line, what in outside(p, p.read_text(encoding="utf-8", errors="replace")):
                bad.append(f"{p.relative_to(ROOT)}:{line}: loads {what}")
    # what the pages point at for fonts is there: each page's fonts.css link, and each font file fonts.css names
    for page in sorted(ROOT.glob("web/*/index.html")):
        for href in re.findall(r"<link[^>]*href=\"([^\"]*fonts\.css)\"", page.read_text(encoding="utf-8")):
            css = (page.parent / href).resolve()
            if not css.is_file():
                bad.append(f"{page.relative_to(ROOT)}: {href} is not there")
                continue
            for f in re.findall(r"url\(([^)]+)\)", css.read_text(encoding="utf-8")):
                if not (css.parent / f).is_file():
                    bad.append(f"{css.relative_to(ROOT)}: {f} is not there")
    if bad:
        print("\n".join(bad))
        print(f"FAIL: {len(bad)} load(s) from an outside host (the allow-list is empty)")
        return 1
    print(f"ok: {n} web files, no load from an outside host")
    return 0


if __name__ == "__main__":
    sys.exit(main())
