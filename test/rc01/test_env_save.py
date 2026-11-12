#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""RC01 UB03 host regression: verified NAND ENV save and web ENV feedback.

Three fixtures, all running the real code:

  * env_save_fixture.c: the complete real common/env_nand.c is #included
    (its own CMD_SAVEENV/SPL/redundant preprocessor structure compiles as
    is) together with the real nand_erase_opts/nand_check_range_capacity
    and the nand.h inlines, extracted by symbol.  Only the NAND medium,
    env_export (deterministic content, real CRC32) and env_import are
    simulated.  Built for the RC01 64 KiB format in a 4 block partition and
    for a 256 KiB format that spans two erase blocks in an 8 block
    partition, plus the redundant configuration; a compile-only matrix
    covers the SPL and no-saveenv configurations.
  * env_set_fixture.c: the real httpd_handle_env_set() of
    failsafe/failsafe_httpd.c with setenv/saveenv/reset_key cache stubs.
  * env_web_fixture.js: the real inline scripts of about.html and
    term.html with the real common.js in Node (minimal DOM, XHR/fetch
    stubs); terminal commands go through a model of the hush list runner.

Every fixture is also run against the baseline commit (the tree before
UB03) and against minimal single-guard mutations of the current tree; the
listed scenarios must FAIL there, which shows they detect the defects.
Each scenario runs in its own process, so a sanitizer abort on the
baseline (buffer overflow) only fails that scenario.

Run (from anywhere):
  UV_CACHE_DIR=/tmp/uv-cache uv run --offline --no-project \\
      python -B test/rc01/test_env_save.py [--keep] [--verbose]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_nand_write_safety as ub01  # noqa: E402  (symbol extraction)

ROOT = ub01.ROOT
# UB02, accepted; the last tree before UB03.
BASELINE_REV = "603ce5954c17eaed43d918707d3dd7d6411e6942"

ENV_NAND = "common/env_nand.c"
NAND_H = "include/nand.h"
NAND_UTIL = "drivers/mtd/nand/nand_util.c"
FAILSAFE = "failsafe/failsafe_httpd.c"
COMMON_JS = "failsafe/html/pig/common.js"
ABOUT = "failsafe/html/pig/about.html"
TERM = "failsafe/html/pig/term.html"

ENV_FIXTURE = HERE / "env_save_fixture.c"
SET_FIXTURE = HERE / "env_set_fixture.c"
WEB_FIXTURE = HERE / "env_web_fixture.js"

STUB_HEADERS = ["common.h", "command.h", "environment.h", "linux/stddef.h",
                "malloc.h", "memalign.h", "nand.h", "search.h"]

SAVE = ["CONFIG_CMD_SAVEENV", "CONFIG_CMD_NAND"]
ENV_CONFIGS = {
    # RC01: 64 KiB format, 512 KiB APPSBLENV, 128 KiB blocks
    "fmt64": SAVE + ["ENV_FMT=0x10000", "PART_NB=4"],
    # 256 KiB format across two erase blocks, 1 MiB partition
    "fmt256": SAVE + ["ENV_FMT=0x40000", "PART_NB=8"],
}
REDUND_CONFIG = SAVE + ["ENV_FMT=0x10000", "PART_NB=4",
                        "CONFIG_ENV_OFFSET_REDUND"]
COMPILE_ONLY = {
    "spl_no_save": ["CONFIG_SPL_BUILD", "ENV_FMT=0x10000", "PART_NB=4"],
    "spl_save": ["CONFIG_SPL_BUILD"] + SAVE + ["ENV_FMT=0x10000", "PART_NB=4"],
    "no_save": ["CONFIG_CMD_NAND", "ENV_FMT=0x10000", "PART_NB=4"],
}

# Scenarios the baseline must fail (original defects).  Insufficient
# capacity of a single block format means no good block at all (nothing to
# erase); the erase-then-fail defect needs a multi-block format.
CAPACITY_DEFECTS = ["t_precheck_capacity", "t_device_end_capacity"]
ENV_BASELINE_DEFECTS = [
    "t_unaligned_physical_area",
    "t_unaligned_physical_area_first_bad",
    "t_precheck_tail_query_error",   # erased before the query error
    "t_write_short",                 # short write reported OK
    "t_verify_mismatch",             # no read back
    "t_verify_read_ebadmsg",
    "t_verify_read_eio",
    "t_verify_short_read",
    "t_verify_zero_read",
    "t_verify_corrupt_read",
    "t_verify_euclean_mismatch",
    "t_walk_query_error",            # query error treated as bad block
]
SET_BASELINE_DEFECTS = [
    "t_setenv_fail_no_save",
    "t_setenv_fail_reset_key",
    "t_save_fail_not_ok",
    "t_reset_key_save_fail_cache",
    "t_long_name_rejected",          # stack overflow of name[64]
    "t_long_value_rejected",         # stack overflow of value[128]
    "t_long_name_with_amp_rejected",
]
WEB_ACTIONS = ["config_restore", "config_modify", "reset_key_restore",
               "reset_key_modify"]
WEB_FAILURES = ["error", "save_failed", "http500", "network", "timeout"]
WEB_BASELINE_DEFECTS = (
    ["about_%s_%s" % (a, o) for a in WEB_ACTIONS
     for o in ["error", "save_failed", "http500"]]
    + ["term_%s_first" % a for a in
       ["modify", "delete", "reset_one", "reset_all"]])

# Single-guard reverts of the current tree:
# (name, file, old, new, fixture/config, scenarios that must then fail)
MUTATIONS = [
    ("no_precheck", ENV_NAND,
     "ret = nand_check_range_capacity(nand, opts->offset, span,\n"
     "\t\t\t\t\tCONFIG_ENV_RANGE);", "ret = 0;", "env:fmt256",
     ["t_precheck_tail_query_error"] + CAPACITY_DEFECTS),
    ("no_compare", ENV_NAND,
     "if (!ret && memcmp(env_new, env_check, CONFIG_ENV_RANGE)) {",
     "if (0) {", "env:fmt256",
     ["t_verify_mismatch", "t_verify_corrupt_read",
      "t_verify_euclean_mismatch"]),
    ("no_readback", ENV_NAND,
     "ret = env_nand_xfer(opts->offset, span, env_check, 0);",
     "memcpy(env_check, env_new, CONFIG_ENV_RANGE);", "env:fmt64",
     ["t_verify_mismatch", "t_verify_read_ebadmsg", "t_verify_short_read",
      "t_verify_zero_read", "t_verify_corrupt_read"]),
    ("accept_short", ENV_NAND, "if (ret || len != req) {", "if (ret) {",
     "env:fmt256",
     # a short read back is also caught by the compare (second guard)
     ["t_write_short", "t_write_zero", "t_import_short_read",
      "t_import_zero_read"]),
    ("euclean_error", ENV_NAND,
     "\t\t\tif (ret == -EUCLEAN)\n\t\t\t\tret = 0;\n", "", "env:fmt64",
     ["t_verify_euclean_ok", "t_import_euclean"]),
    ("query_as_bad", ENV_NAND,
     "\t\tif (ret < 0) {\n\t\t\tprintf(\"Bad block query",
     "\t\tif (0) {\n\t\t\tprintf(\"Bad block query", "env:fmt64",
     ["t_walk_query_error", "t_import_query_error"]),
    ("fixed_chunk", ENV_NAND,
     "len = min(total - done, (size_t)blocksize);",
     "len = blocksize;", "env:fmt64",
     ["t_save_ok", "t_import_bad_blocks"]),
    ("late_check_alloc", ENV_NAND,
     "\tif (!env_check) {\n\t\tprintf(\"Error: Cannot allocate %d bytes\\n\", "
     "CONFIG_ENV_SIZE);\n\t\tfree(env_new);\n\t\treturn 1;\n\t}\n",
     "", "env:fmt64", ["t_alloc_fail_check_buffer"]),
    ("save_after_set_fail", FAILSAFE,
     "if (setenv(name, value[0] ? value : NULL)) {",
     "if (setenv(name, value[0] ? value : NULL), 0) {", "set",
     ["t_setenv_fail_no_save", "t_setenv_fail_reset_key"]),
    ("ok_on_save_fail", FAILSAFE, 'result = "save_failed";',
     'result = "ok";', "set",
     ["t_save_fail_not_ok", "t_reset_key_save_fail_cache"]),
    ("cache_only_on_save", FAILSAFE,
     "\t\t\t\t\tif (strcmp(name, \"reset_key\") == 0)\n"
     "\t\t\t\t\t\tenv_reset_gpio_invalidate();\n"
     "\t\t\t\t\tif (saveenv()) {",
     "\t\t\t\t\tif (saveenv()) {", "set",
     ["t_reset_key_save_fail_cache", "t_set_reset_key_ok"]),
    ("no_length_check", FAILSAFE,
     "\t\t    (size_t)(eq - body) < sizeof(name) &&\n"
     "\t\t    (size_t)(end - eq - 1) < sizeof(value)) {",
     "\t\t    1) {", "set",
     ["t_long_name_rejected", "t_long_value_rejected",
      "t_long_name_with_amp_rejected"]),
    ("about_ignores_body", ABOUT, "if (r !== 'ok')\n\t\t\t\talert(",
     "if (false)\n\t\t\t\talert(", "web",
     ["about_%s_%s" % (a, o) for a in WEB_ACTIONS for o in WEB_FAILURES]),
    ("about_no_network", ABOUT,
     "xhr.onerror = xhr.ontimeout = xhr.onabort = function() { finish(null); };",
     "", "web",
     ["about_%s_%s" % (a, o) for a in WEB_ACTIONS
      for o in ("network", "timeout")]),
    ("term_unconditional", TERM, "var SAVE_TAIL = ' && saveenv';",
     "var SAVE_TAIL = '; saveenv';", "web",
     ["term_%s_first" % a for a in
      ["modify", "delete", "reset_one", "reset_all"]]),
]


# ---------------------------------------------------------------- building

def stub_dir(work):
    d = work / "stubinc"
    for h in STUB_HEADERS:
        p = d / h
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("/* empty host stub, types come from the fixture */\n")
    return d


def env_source(work, tag, get, label):
    """Fixture C text with the extracted NAND pieces and the env_nand.c
    path (a copy when the tree is not the working tree)."""
    nh, nu = get(NAND_H), get(NAND_UTIL)
    out = []
    out.append(ub01.frag(label(NAND_H), ub01.struct(nh, "nand_erase_options")))
    out.append(ub01.frag(label(NAND_H), ub01.typedef(nh, "nand_erase_options_t")))
    for f in ("nand_read", "nand_write", "nand_block_isbad"):
        out.append(ub01.frag(label(NAND_H), ub01.function(nh, f)))
    out.append(ub01.frag(label(NAND_H),
                         ub01.prototype(nh, "nand_check_range_capacity")))
    # nand_read_skip_bad: used by the baseline readenv
    for f in ("nand_erase_opts", "check_skip_len",
              "nand_check_range_capacity", "nand_read_skip_bad"):
        out.append(ub01.frag(label(NAND_UTIL), ub01.function(nu, f)))
    env_path = work / ("env_nand_%s.c" % tag)
    env_path.write_text(get(ENV_NAND))
    fx = ENV_FIXTURE.read_text()
    marker = "/* @@EXTRACTED@@ */"
    idx = fx.index(marker)
    after = ub01.line_of(fx, idx) + 1
    return ('#line 1 "%s"\n' % ENV_FIXTURE + fx[:idx] + "\n" + "".join(out)
            + '#line %d "%s"\n' % (after, ENV_FIXTURE)
            + fx[idx + len(marker):]), env_path


def defines(defs):
    return ["-D" + (d if "=" in d else d + "=1") for d in defs]


def compile_c(work, name, code, extra, sanitize=True, syntax_only=False):
    c = work / (name + ".c")
    exe = work / name
    c.write_text(code)
    cc = os.environ.get("CC", "cc")
    base = [cc, "-std=gnu11", "-O1", "-g", "-no-pie", "-Wall",
            "-Werror=implicit-function-declaration",
            "-Werror=unused-function",
            "-Wno-unused-variable", "-Wno-unused-but-set-variable",
            "-Wno-format", "-Wno-pointer-to-int-cast",
            "-Wno-int-to-pointer-cast", "-Wno-address-of-packed-member"]
    base += extra
    if syntax_only:
        cmd = base + ["-fsyntax-only", str(c)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return r
    san = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer",
           "-fno-sanitize-recover=undefined"] if sanitize else []
    r = subprocess.run(base + san + ["-o", str(exe), str(c)],
                       capture_output=True, text=True)
    if r.returncode != 0 and san:
        print("note: sanitizer build failed, building without sanitizers")
        r = subprocess.run(base + ["-o", str(exe), str(c)],
                           capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stderr)
        raise SystemExit("compile failed: %s" % c)
    return exe


def run_each(cmd, verbose):
    """Run every listed scenario in its own process; return {name: result}."""
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    r = subprocess.run(cmd + ["--list"], capture_output=True, text=True,
                       timeout=60, env=env)
    names = [l.strip() for l in r.stderr.splitlines() if l.strip()]
    if r.returncode != 0 or not names:
        sys.stdout.write(r.stdout + r.stderr)
        raise SystemExit("cannot list scenarios: %s" % cmd)
    results = {}
    for n in names:
        p = subprocess.run(cmd + [n], capture_output=True, text=True,
                           timeout=120, env=env)
        res = "FAIL"
        for line in p.stderr.splitlines():
            if line.strip() == "PASS " + n and p.returncode == 0:
                res = "PASS"
        results[n] = res
        if verbose or (res == "FAIL" and verbose is None):
            sys.stdout.write(p.stdout[-3000:])
            if res == "FAIL":
                sys.stdout.write(p.stderr[-2000:])
    return results


def build_env(work, tag, defs, get, label):
    code, env_path = env_source(work, tag, get, label)
    extra = defines(defs) + ['-DENV_NAND_C="%s"' % env_path,
                             "-I", str(stub_dir(work))]
    return compile_c(work, "env_" + tag, code, extra)


def build_set(work, tag, get, label):
    fx = SET_FIXTURE.read_text()
    marker = "/* @@EXTRACTED@@ */"
    idx = fx.index(marker)
    after = ub01.line_of(fx, idx) + 1
    fn = ub01.frag(label(FAILSAFE),
                   ub01.function(get(FAILSAFE), "httpd_handle_env_set"))
    code = ('#line 1 "%s"\n' % SET_FIXTURE + fx[:idx] + "\n" + fn
            + '#line %d "%s"\n' % (after, SET_FIXTURE)
            + fx[idx + len(marker):])
    return compile_c(work, "set_" + tag, code, [])


def web_cmd(work, tag, get):
    d = work / ("web_" + tag)
    d.mkdir(exist_ok=True)
    paths = []
    for p in (COMMON_JS, ABOUT, TERM):
        dst = d / Path(p).name
        dst.write_text(get(p))
        paths.append(str(dst))
    return ["node", str(WEB_FIXTURE)] + paths


def summary(label, res):
    passed = sum(v == "PASS" for v in res.values())
    print("%-34s %d/%d scenarios passed" % (label, passed, len(res)))
    for n, v in res.items():
        if v != "PASS":
            print("    FAIL", n)
    return passed == len(res) and len(res) > 0


def expect_fail(label, res, expected):
    caught = [t for t in expected if res.get(t) == "FAIL"]
    for t in expected:
        if t not in caught:
            print("    %-40s NOT DETECTED" % t)
    print("%-34s %d/%d expected failures detected"
          % (label, len(caught), len(expected)))
    return len(caught) == len(expected)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true",
                    help="keep the generated sources and binaries")
    ap.add_argument("--verbose", action="store_true",
                    help="print U-Boot messages from the fixtures")
    args = ap.parse_args()
    verbose = True if args.verbose else None

    if not shutil.which("node"):
        print("error: node is required for the web page scenarios")
        print("RESULT: FAILED")
        return 1

    work = Path(tempfile.mkdtemp(prefix="ub03-env-"))
    tree = ub01.read_tree
    here = lambda p: str(ROOT / p)
    ok = True
    try:
        # --- current tree
        for cfg, defs in ENV_CONFIGS.items():
            exe = build_env(work, cfg, defs, tree, here)
            ok &= summary("env_nand %s" % cfg, run_each([str(exe)], verbose))
        exe = build_env(work, "redund", REDUND_CONFIG, tree, here)
        ok &= summary("env_nand redundant", run_each([str(exe)], verbose))
        for cfg, defs in COMPILE_ONLY.items():
            code, env_path = env_source(work, "co_" + cfg, tree, here)
            r = compile_c(work, "co_" + cfg, code,
                          defines(defs) + ['-DENV_NAND_C="%s"' % env_path,
                                           "-I", str(stub_dir(work))],
                          syntax_only=True)
            good = r.returncode == 0
            print("%-34s %s" % ("compile-only %s" % cfg,
                                "ok" if good else "FAILED"))
            if not good:
                sys.stdout.write(r.stderr[-3000:])
            ok &= good
        exe = build_set(work, "fixed", tree, here)
        ok &= summary("/env_set handler", run_each([str(exe)], verbose))
        ok &= summary("about/term pages (node)",
                      run_each(web_cmd(work, "fixed", tree), verbose))
        from test_env_terminal import run_checks
        ok &= run_checks(work, web_cmd(work, "hush", tree))

        # --- minimal mutations of the current tree
        for name, path, old, new, target, expected in MUTATIONS:
            def get(p, path=path, old=old, new=new, name=name):
                text = ub01.read_tree(p)
                if p == path:
                    if text.count(old) != 1:
                        raise SystemExit("mutation %s: anchor not found" % name)
                    text = text.replace(old, new)
                return text
            tag = "mut_" + name
            if target.startswith("env:"):
                cfg = target[4:]
                cmd = [str(build_env(work, tag, ENV_CONFIGS[cfg], get, here))]
            elif target == "set":
                cmd = [str(build_set(work, tag, get, here))]
            else:
                cmd = web_cmd(work, tag, get)
            ok &= expect_fail("mutation %s" % name, run_each(cmd, False),
                              expected)

        # --- baseline
        if ub01.read_rev(BASELINE_REV, ENV_NAND) is None:
            print("note: baseline %s not available; defect detection "
                  "skipped" % BASELINE_REV[:12])
        else:
            get = lambda p: ub01.read_rev(BASELINE_REV, p)
            base = lambda p: "baseline:" + p
            for cfg, defs in ENV_CONFIGS.items():
                res = run_each([str(build_env(work, "base_" + cfg, defs, get,
                                              base))], False)
                want = ENV_BASELINE_DEFECTS + (
                    CAPACITY_DEFECTS if cfg != "fmt64" else [])
                ok &= expect_fail("baseline env_nand %s" % cfg, res, want)
                other = [t for t, v in res.items()
                         if v == "FAIL" and t not in want]
                if other:
                    print("    baseline also fails:", ", ".join(other))
            res = run_each([str(build_set(work, "base", get, base))], False)
            ok &= expect_fail("baseline /env_set", res, SET_BASELINE_DEFECTS)
            res = run_each(web_cmd(work, "base", get), False)
            ok &= expect_fail("baseline about/term", res,
                              WEB_BASELINE_DEFECTS)
    finally:
        if args.keep:
            print("build dir:", work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    print("RESULT:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
