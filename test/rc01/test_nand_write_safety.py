#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""RC01 UB01 host regression: bounded NAND flash writes.

Extracts the real functions from the U-Boot tree by symbol (C syntax
boundaries, not line numbers), compiles them with the simulated NAND in
nand_write_safety_fixture.c and runs the scenarios.

The API scenarios are additionally built against the baseline commit
(the public four-port revision before UB01) and must FAIL there, which shows that they hit
the original defects.  The baseline write_to_flash is also linked into
the main build to compare the unchanged MMC/NOR command routes.  When the
baseline commit is unavailable these comparisons are skipped and reported.

Run (from anywhere):
  UV_CACHE_DIR=/tmp/uv-cache uv run --offline --no-project \
      python -B test/rc01/test_nand_write_safety.py [--keep] [--verbose]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]  # uboot/
FIXTURE = HERE / "nand_write_safety_fixture.c"
BASELINE_REV = "54a2eb299d190abfabca58fe061bd7dcaa2e6c19"

NAND_H = "include/nand.h"
NAND_UTIL = "drivers/mtd/nand/nand_util.c"
FLASHWRITE = "common/cmd_flashwrite.c"
SMEM = "arch/arm/cpu/armv7/qca/common/smem.c"

HEADER_INLINES = ["nand_read", "nand_write", "nand_block_isbad"]
HEADER_PROTOS = ["nand_read_skip_bad", "nand_write_skip_bad",
                 "nand_erase_opts", "nand_check_range_capacity",
                 "nand_verify"]
UTIL_FUNCS = ["nand_erase_opts", "check_skip_len",
              "nand_check_range_capacity", "drop_ffs", "nand_verify",
              "nand_write_skip_bad", "nand_read_skip_bad"]
BASELINE_API_TESTS = [
    "t_api_post_scan_bad_bounded",
    "t_api_negative_query_fails",
    "t_api_scan_stops_at_lim",
    "t_api_erase_failure_returns",
    "t_api_cleanmarker_failure_returns",
    "t_api_short_write_fails",
]


# ---------------------------------------------------------------- C scanning

def _skip_trivia(src, i):
    """If src[i] starts a comment or literal, return the index after it."""
    if src.startswith("/*", i):
        j = src.find("*/", i + 2)
        if j < 0:
            raise ValueError("unterminated comment")
        return j + 2
    if src.startswith("//", i):
        j = src.find("\n", i)
        return len(src) if j < 0 else j
    if src[i] in "\"'":
        q = src[i]
        j = i + 1
        while src[j] != q:
            j += 2 if src[j] == "\\" else 1
        return j + 1
    return None


def _match(src, i, open_ch, close_ch):
    """src[i] == open_ch; return index of the matching close_ch."""
    depth = 0
    while i < len(src):
        n = _skip_trivia(src, i)
        if n is not None:
            i = n
            continue
        c = src[i]
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unbalanced %s" % open_ch)


def _skip_ws(src, i):
    while True:
        while i < len(src) and src[i].isspace():
            i += 1
        n = _skip_trivia(src, i) if i < len(src) and src[i] == "/" else None
        if n is None:
            return i
        i = n


def _decls(src, name):
    """Yield (start, paren_close) for top-level declarators of name()."""
    pat = re.compile(r"^[A-Za-z_][^;{}()#]*?\b%s\s*\(" % re.escape(name),
                     re.M)
    for m in pat.finditer(src):
        yield m.start(), _match(src, m.end() - 1, "(", ")")


def line_of(src, pos):
    return src.count("\n", 0, pos) + 1


def function(src, name):
    """Return (line, text) of the definition of function name."""
    found = []
    for start, close in _decls(src, name):
        i = _skip_ws(src, close + 1)
        if i < len(src) and src[i] == "{":
            end = _match(src, i, "{", "}")
            found.append((line_of(src, start), src[start:end + 1]))
    if len(found) != 1:
        raise LookupError("%s: %d definitions" % (name, len(found)))
    return found[0]


def prototype(src, name):
    found = []
    for start, close in _decls(src, name):
        i = _skip_ws(src, close + 1)
        if i < len(src) and src[i] == ";":
            found.append((line_of(src, start), src[start:i + 1]))
    if len(found) != 1:
        raise LookupError("%s: %d prototypes" % (name, len(found)))
    return found[0]


def struct(src, tag):
    m = re.search(r"^struct\s+%s\s*\{" % re.escape(tag), src, re.M)
    if not m:
        raise LookupError("struct " + tag)
    end = _match(src, m.end() - 1, "{", "}")
    semi = src.index(";", end)
    return line_of(src, m.start()), src[m.start():semi + 1]


def macro(src, name):
    m = re.search(r"^#define\s+%s\b.*$" % re.escape(name), src, re.M)
    if not m:
        raise LookupError("macro " + name)
    return line_of(src, m.start()), m.group(0)


def typedef(src, alias):
    m = re.search(r"^typedef\s+[^;]*\b%s\s*;" % re.escape(alias), src, re.M)
    if not m:
        raise LookupError("typedef " + alias)
    return line_of(src, m.start()), m.group(0)


# ---------------------------------------------------------------- sources

def read_tree(path):
    return (ROOT / path).read_text()


def read_rev(rev, path):
    r = subprocess.run(["git", "-C", str(ROOT), "show", "%s:%s" % (rev, path)],
                       capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def frag(label, item, rename=None):
    line, text = item
    if rename:
        text = re.sub(r"\b%s\b" % rename[0], rename[1], text, count=1)
    return '#line %d "%s"\n%s\n' % (line, label, text)


def extract(get, label, util_funcs, protos, flash=True, base_flash=None):
    h, u = get(NAND_H), get(NAND_UTIL)
    out = []
    out.append(frag(label(NAND_H), struct(h, "nand_erase_options")))
    out.append(frag(label(NAND_H), typedef(h, "nand_erase_options_t")))
    for m in ("WITH_DROP_FFS", "WITH_WR_VERIFY"):
        out.append(frag(label(NAND_H), macro(h, m)))
    for f in HEADER_INLINES:
        out.append(frag(label(NAND_H), function(h, f)))
    for p in protos:
        out.append(frag(label(NAND_H), prototype(h, p)))
    for f in util_funcs:
        out.append(frag(label(NAND_UTIL), function(u, f)))
    if flash:
        out.append(frag(label(SMEM), function(get(SMEM), "getpart_offset_size")))
        fw = get(FLASHWRITE)
        for f in ("write_to_flash", "fl_erase", "do_flash"):
            out.append(frag(label(FLASHWRITE), function(fw, f)))
    if base_flash:
        out.append("#define HAVE_BASE_WRITE_TO_FLASH 1\n")
        out.append(frag("baseline:" + FLASHWRITE,
                        function(base_flash, "write_to_flash"),
                        ("write_to_flash", "base_write_to_flash")))
    return "".join(out)


def generate(extracted, defines=()):
    fx = FIXTURE.read_text()
    marker = "/* @@EXTRACTED@@ */"
    idx = fx.index(marker)
    after = line_of(fx, idx) + 1
    head = "".join("#define %s 1\n" % d for d in defines)
    return (head + '#line 1 "%s"\n' % FIXTURE + fx[:idx] + "\n" + extracted
            + '#line %d "%s"\n' % (after, FIXTURE) + fx[idx + len(marker):])


def build(work, name, code):
    c = work / (name + ".c")
    exe = work / name
    c.write_text(code)
    cc = os.environ.get("CC", "cc")
    base = [cc, "-std=gnu11", "-O1", "-g", "-no-pie", "-Wall",
            "-Wno-unused-function", "-Wno-unused-variable",
            "-Wno-unused-but-set-variable", "-Wno-format",
            "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
            "-o", str(exe), str(c)]
    san = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer",
           "-fno-sanitize-recover=undefined"]
    r = subprocess.run(base[:1] + san + base[1:], capture_output=True,
                       text=True)
    if r.returncode != 0:
        print("note: sanitizer build failed, building without sanitizers")
        r = subprocess.run(base, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stderr)
        raise SystemExit("compile failed: %s" % c)
    errors = [l for l in r.stderr.splitlines() if "error" in l]
    if errors:
        sys.stdout.write(r.stderr)
    return exe


def run(exe, verbose):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    r = subprocess.run([str(exe)], capture_output=True, text=True,
                       timeout=120, env=env)
    results = {}
    for line in r.stderr.splitlines():
        m = re.match(r"(PASS|FAIL) (\S+)$", line)
        if m:
            results[m.group(2)] = m.group(1)
    if verbose:
        sys.stdout.write(r.stdout)
    return r, results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true",
                    help="keep the generated sources and binaries")
    ap.add_argument("--verbose", action="store_true",
                    help="print U-Boot messages from the fixture")
    args = ap.parse_args()

    work = Path(tempfile.mkdtemp(prefix="ub01-nand-"))
    ok = True
    try:
        base_flash = read_rev(BASELINE_REV, FLASHWRITE)
        code = generate(extract(read_tree, lambda p: str(ROOT / p),
                                UTIL_FUNCS, HEADER_PROTOS,
                                base_flash=base_flash))
        exe = build(work, "fixed", code)
        r, res = run(exe, args.verbose)
        sys.stdout.write(r.stderr)
        if r.returncode != 0 or not res or "FAIL" in res.values():
            ok = False
            if not args.verbose:
                sys.stdout.write(r.stdout[-4000:])
        print("fixed tree: %d/%d scenarios passed"
              % (sum(v == "PASS" for v in res.values()), len(res)))
        if base_flash is None:
            print("note: baseline %s not available; route comparison "
                  "used literal commands only" % BASELINE_REV[:12])

        # Exercise the real entry with layout preparation before page rounding.
        code = generate(extract(read_tree, lambda p: str(ROOT / p),
                                UTIL_FUNCS, HEADER_PROTOS,
                                base_flash=base_flash),
                        defines=("CONFIG_IPQ806X",))
        r, res = run(build(work, "layout", code), args.verbose)
        sys.stdout.write(r.stderr)
        if r.returncode != 0 or not res or "FAIL" in res.values():
            ok = False
            if not args.verbose:
                sys.stdout.write(r.stdout[-4000:])
        print("IPQ806X entry/layout: %d/%d scenarios passed"
              % (sum(v == "PASS" for v in res.values()), len(res)))

        base_util = read_rev(BASELINE_REV, NAND_UTIL)
        if base_util is None:
            print("note: baseline unavailable; mutation check skipped")
        else:
            get = lambda p: read_rev(BASELINE_REV, p)
            code = generate(
                extract(get, lambda p: "baseline:" + p,
                        [f for f in UTIL_FUNCS
                         if f != "nand_check_range_capacity"],
                        [p for p in HEADER_PROTOS
                         if p != "nand_check_range_capacity"],
                        flash=False),
                defines=("BASELINE",))
            exe = build(work, "baseline", code)
            r, res = run(exe, False)
            caught = [t for t in BASELINE_API_TESTS if res.get(t) == "FAIL"]
            for t in BASELINE_API_TESTS:
                print("baseline %-38s %s" % (t, "defect detected"
                      if t in caught else "NOT DETECTED"))
            if len(caught) != len(BASELINE_API_TESTS):
                ok = False
            print("baseline %s: %d/%d defects detected"
                  % (BASELINE_REV[:12], len(caught), len(BASELINE_API_TESTS)))
    finally:
        if args.keep:
            print("build dir:", work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    print("RESULT:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
