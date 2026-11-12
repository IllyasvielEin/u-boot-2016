#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run real page-generated ENV commands through the full production hush parser."""
import json
import os
import re
import subprocess
from pathlib import Path

import test_nand_write_safety as ex

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BASELINE = "fe4d491dafe7ad01ff3e0196496539cb071f59e1"
NVEDIT = "common/cmd_nvedit.c"


def build(work, name, nvedit):
    stub = work / "hush-stubinc"
    for h in ["common.h", "command.h", "console.h", "bootretry.h", "cli.h",
              "malloc.h", "linux/ctype.h"]:
        p = stub / h
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("/* host types supplied by fixture */\n")
    (stub / "cli_hush.h").write_text((ROOT / "include/cli_hush.h").read_text())
    parts = ['#include "%s"\n' % (ROOT / "common/cli_hush.c")]
    for path, funcs in [
        (NVEDIT, ["_do_env_set", "do_env_set", "do_env_default"]),
        ("common/command.c", ["cmd_call", "cmd_process"]),
        ("common/cli.c", ["run_command"]),
    ]:
        source = nvedit if path == NVEDIT else (ROOT / path).read_text()
        parts.extend(ex.frag(str(ROOT / path), ex.function(source, fn)) for fn in funcs)
    c = work / (name + ".c")
    c.write_text((HERE / "env_hush_fixture.c").read_text().replace(
        "/* @@EXTRACTED@@ */", "".join(parts)))
    exe = work / name
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g",
                    "-no-pie", "-Werror=implicit-function-declaration",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                    "-I" + str(stub), str(c), "-o", str(exe)],
                   check=True, capture_output=True, text=True)
    return exe


def execute(exe, command, exists=1, first_fail=0, save_fail=0):
    r = subprocess.run([str(exe), command, str(exists), str(first_fail), str(save_fail)],
                       capture_output=True, text=True, timeout=20,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    m = re.search(r"RESULT rc=(-?\d+) saves=(\d+) exists=(\d+)", r.stderr)
    assert r.returncode == 0 and m, r.stdout + r.stderr
    return int(m[1]), int(m[2]), r.stdout


def run_checks(work, web_command):
    r = subprocess.run(web_command + ["--commands"], capture_output=True,
                       text=True, check=True)
    commands = json.loads(r.stdout)
    exe = build(work, "env-terminal", (ROOT / NVEDIT).read_text())
    checks = 0
    for command in commands.values():
        rc, saves, output = execute(exe, command)
        assert rc == 0 and saves == 1 and "OK" in output, (command, output)
        checks += 1
        rc, saves, output = execute(exe, command, save_fail=1)
        assert rc != 0 and saves == 1 and "FAILED!" in output, (command, output)
        checks += 1
    for exists, first_fail in [(0, 0), (1, 1)]:
        rc, saves, output = execute(exe, commands["delete"], exists, first_fail)
        assert rc != 0 and saves == 0 and "Error: failed to delete" in output, output
        assert "Saving Environment" not in output, output
        checks += 1
    print("real hush page commands            %d/%d checks passed" % (checks, checks))
    baseline = ex.read_rev(BASELINE, NVEDIT)
    if baseline is None:
        print("note: baseline terminal diagnostic unavailable")
    else:
        old = build(work, "env-terminal-baseline", baseline)
        rc, saves, output = execute(old, commands["delete"], exists=0)
        assert rc != 0 and saves == 0 and output == "", output
        print("baseline terminal diagnostic       silent failure detected")
    return True
