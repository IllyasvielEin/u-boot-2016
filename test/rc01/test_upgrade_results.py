#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""RC01 UB02 host regression: upgrade results, command status, rootfs targets.

Extracts the real functions by symbol from the U-Boot tree:

  * hush control chain: run_command -> parse_stream_outer -> run_list ->
    run_list_real, and cmd_process -> cmd_call (common/cli*.c, command.c);
  * the "flash" command (do_flash, write_to_flash, UBI fallback) and the
    SMEM partition lookup;
  * the "ubi" command (do_ubi, ubi_create_vol, remove/write/lookup helpers);
  * the SYSUPGRADE UBI writer (lib/sysupgrade_parser.c);
  * the web upgrade handlers (net/httpd.c) and the outer web dispatcher
    failsafe_httpd_poll (failsafe/failsafe_httpd.c);
  * the UB01 NAND capacity check.

Only the media and low level drivers are simulated (upgrade_results_fixture.c):
NAND bad block queries/erase/write, the UBI volume table with the
lowest-free-ID allocation of drivers/mtd/ubi/vmt.c, "nand device", mmc/sf.
Command strings go through a small tokenizer into the real hush list
runner, so negative errno values returned by the real commands reach the
real hush "exit" handling.

Every configuration is also built from the UB01 baseline commit; the listed
scenarios must FAIL there, which shows that they detect the original
defects.

Run (from anywhere):
  UV_CACHE_DIR=/tmp/uv-cache uv run --offline --no-project \\
      python -B test/rc01/test_upgrade_results.py [--keep] [--verbose]
"""

import argparse
import re
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_nand_write_safety as ub01  # noqa: E402  (symbol extraction)

ROOT = ub01.ROOT
FIXTURE = HERE / "upgrade_results_fixture.c"
# UB01, accepted; the last tree before UB02.
BASELINE_REV = "e2b58fad47cf4e88a80a85a9d952b39c74bdbbe6"

HUSH = "common/cli_hush.c"
CLI = "common/cli.c"
COMMAND = "common/command.c"
FLASHWRITE = "common/cmd_flashwrite.c"
CMD_UBI = "common/cmd_ubi.c"
SMEM = "arch/arm/cpu/armv7/qca/common/smem.c"
NAND_H = "include/nand.h"
NAND_UTIL = "drivers/mtd/nand/nand_util.c"
SYSUPGRADE = "lib/sysupgrade_parser.c"
SYSUPGRADE_H = "include/sysupgrade_parser.h"
HTTPD = "net/httpd.c"
FAILSAFE = "failsafe/failsafe_httpd.c"
IPQ_API = "include/ipq_api.h"

IPQ_API_MACROS = [
    "WEBFAILSAFE_PROGRESS_START", "WEBFAILSAFE_PROGRESS_UPLOADING",
    "WEBFAILSAFE_PROGRESS_UPLOAD_READY", "WEBFAILSAFE_PROGRESS_UPGRADING",
    "WEBFAILSAFE_PROGRESS_UPGRADE_READY",
    "WEBFAILSAFE_PROGRESS_UPGRADE_FAILED",
    "WEBFAILSAFE_UPGRADE_TYPE_FIRMWARE", "WEBFAILSAFE_UPGRADE_TYPE_UBOOT",
    "WEBFAILSAFE_UPGRADE_TYPE_ART", "WEBFAILSAFE_UPGRADE_TYPE_IMG",
    "WEBFAILSAFE_UPGRADE_TYPE_CDT", "WEBFAILSAFE_UPGRADE_TYPE_MIBIB",
    "WEBFAILSAFE_UPGRADE_TYPE_PTABLE", "WEBFAILSAFE_UPGRADE_TYPE_INITRAMFS",
    "IMG_FLASH_NOR", "IMG_FLASH_NAND", "IMG_FLASH_EMMC", "IMG_FLASH_NAND_RAW",
    "MIBIB_NAME", "CDT_NAME", "CDT_NAME_1", "UBOOT_NAME", "UBOOT_NAME_1",
    "ART_NAME", "ROOTFS_NAME0", "ROOTFS_NAME1", "ROOTFS_NAME2",
    "ROOTFS_NAME_1",
]
HTTPD_HANDLERS = [
    "do_uboot_upgrade", "do_art_upgrade", "do_gpt_upgrade",
    "do_img_upgrade", "do_cdt_upgrade", "do_mibib_upgrade",
    "do_ptable_upgrade", "do_initramfs_boot",
]

# Scenarios that must fail on the baseline (original defects).
BASELINE_DEFECTS = {
    "rc01": [
        "t_flash_missing_part_fails_in_hush",
        "t_flash_invalid_part_fails_in_hush",
        "t_ubi_create_errno_stops_chain",
        "t_web_positive_failure_no_reset",
        "t_raw_rc01_single_rootfs_once",
        "t_raw_rc01_backup_missing_rejected",
        "t_sysupgrade_rc01_backup_rejected",
        "t_sysupgrade_create_errno_fails",
        "t_sysupgrade_attach_failure",
        "t_sysupgrade_remove_failure",
        "t_sysupgrade_data_create_failure",
        "t_sysupgrade_rc01_volume_ids_checked",
    ],
    "other": [
        "t_flash_missing_part_fails_in_hush",
        "t_ubi_create_errno_stops_chain",
        "t_web_positive_failure_no_reset",
        "t_raw_other_backup_missing_rejected",
        "t_sysupgrade_backup_failure_fails",
        "t_sysupgrade_data_create_failure",
        "t_web_nor_failure_no_reset",
        "t_flash_nor_missing_part_fails",
    ],
}

# Single-fix reverts on the current tree: (name, file, old, new, config,
# scenarios that must then fail).  "raw_chain" keeps the errno
# normalization but restores the unconditional rootfs/rootfs1/rootfs2 chain:
# a normal single rootfs upgrade must then be reported as failed.
MUTATIONS = [
    ("raw_chain", HTTPD,
     "return raw_ubi_nand_upgrade(size);", ";", "rc01",
     ["t_raw_rc01_single_rootfs_once"]),
    ("raw_chain", HTTPD,
     "return raw_ubi_nand_upgrade(size);", ";", "other",
     ["t_raw_other_single_rootfs"]),
    ("flash_errno", FLASHWRITE,
     "ret != CMD_RET_SUCCESS && ret != CMD_RET_USAGE", "0", "rc01",
     ["t_flash_missing_part_fails_in_hush",
      "t_flash_invalid_part_fails_in_hush"]),
    ("create_errno", CMD_UBI,
     "return CMD_RET_FAILURE;\n\t\t\t}\n\t\t\treturn CMD_RET_SUCCESS;",
     "return err;\n\t\t\t}\n\t\t\treturn CMD_RET_SUCCESS;", "rc01",
     ["t_ubi_create_errno_stops_chain", "t_sysupgrade_create_errno_fails"]),
    ("web_dispatch", FAILSAFE,
     "webfailsafe_upgrade_type) != 0)", "webfailsafe_upgrade_type) < 0)",
     "rc01", ["t_web_positive_failure_no_reset"]),
    ("tar_backup", SYSUPGRADE,
     "if (backup_enabled) {\n\t\tprintf(\"SYSUPGRADE backup",
     "if (0) {\n\t\tprintf(\"SYSUPGRADE backup", "rc01",
     ["t_sysupgrade_rc01_backup_rejected"]),
]

CONFIGS = {
    # RC01: pure NAND, UBI fallback in "flash", no eMMC.
    "rc01": ["CONFIG_IPQ9574_XIAOMI_BE10000"],
    # Another pure NAND board with eMMC support compiled in (MMC/NOR routes).
    "other": ["CONFIG_QCA_MMC"],
}


def hush_types(h):
    out = []
    for m in ("FLAG_EXIT_FROM_LOOP", "FLAG_PARSE_SEMICOLON",
              "FLAG_REPARSING"):
        out.append(ub01.frag(HUSH, ub01.macro(h, m)))
    for t in ("pipe_style", "reserved_style"):
        out.append(ub01.frag(HUSH, ub01.typedef(h, t)))
    for s in ("p_context", "child_prog", "pipe", "in_str"):
        out.append(ub01.frag(HUSH, ub01.struct(h, s)))
    return "".join(out)


def extract(get, label, fixed):
    """Return (types, functions) C text from the tree read by get()."""
    h = get(HUSH)
    types = [hush_types(h)]
    nh = get(NAND_H)
    types.append(ub01.frag(label(NAND_H), ub01.struct(nh, "nand_erase_options")))
    types.append(ub01.frag(label(NAND_H), ub01.typedef(nh, "nand_erase_options_t")))
    types.append(ub01.frag(label(NAND_H), ub01.macro(nh, "WITH_WR_VERIFY")))
    api = get(IPQ_API)
    for m in IPQ_API_MACROS:
        types.append(ub01.frag(label(IPQ_API), ub01.macro(api, m)))
    types.append(ub01.frag(label(SYSUPGRADE_H),
                           ub01.macro(get(SYSUPGRADE_H),
                                      "sysupgrade_parts_valid")))
    ubi = get(CMD_UBI)
    types.append(ub01.frag(label(CMD_UBI), ub01.struct(ubi, "selected_dev")))

    fn = []
    add = lambda path, src, names: fn.extend(
        ub01.frag(label(path), ub01.function(src, n)) for n in names)
    add(NAND_H, nh, ["nand_block_isbad"])
    add(NAND_UTIL, get(NAND_UTIL), ["nand_check_range_capacity"])
    add(SMEM, get(SMEM), ["getpart_offset_size"])
    add(COMMAND, get(COMMAND), ["cmd_call", "cmd_process"])
    add(HUSH, h, ["run_list_real", "run_list", "parse_stream_outer"])
    add(CLI, get(CLI), ["run_command"])
    add(CMD_UBI, ubi, ["ubi_check_volumename", "ubi_check",
                       "verify_mkvol_req", "ubi_create_vol",
                       "ubi_find_volume", "ubi_remove_vol",
                       "ubi_volume_continue_write", "ubi_volume_begin_write",
                       "ubi_volume_write"]
        + (["ubi_get_volume_id"] if fixed else []) + ["do_ubi"])
    add(FLASHWRITE, get(FLASHWRITE), ["write_to_flash", "fl_erase",
                                      "ubi_vol_present", "write_ubi_vol",
                                      "do_flash"])
    sp = get(SYSUPGRADE)
    add(SYSUPGRADE, sp, ["sysupgrade_ubi_init", "ubi_write_volume"])
    if fixed:
        fn.append("#ifdef CONFIG_IPQ9574_XIAOMI_BE10000\n")
        add(SYSUPGRADE, sp, ["rc01_check_volume_ids"])
        fn.append("#endif\n")
    add(SYSUPGRADE, sp, ["sysupgrade_write_ubi_volumes"])
    hd = get(HTTPD)
    for n in ["do_firmware_upgrade"] + HTTPD_HANDLERS:
        fn.append(ub01.frag(label(HTTPD), ub01.prototype(hd, n)))
    add(HTTPD, hd, ["execute_command", "print_upgrade_warning",
                    "fw_type_to_string"])
    if fixed:
        add(HTTPD, hd, ["raw_ubi_nand_upgrade"])
    add(HTTPD, hd, ["do_http_upgrade", "do_firmware_upgrade"]
        + HTTPD_HANDLERS)
    add(FAILSAFE, get(FAILSAFE), ["failsafe_httpd_poll"])
    return "".join(types), "".join(fn)


def generate(types, funcs, defines):
    fx = FIXTURE.read_text()
    out = "".join("#define %s 1\n" % d for d in defines)
    pos = 0
    out += '#line 1 "%s"\n' % FIXTURE
    for marker, text in (("/* @@TYPES@@ */", types),
                         ("/* @@EXTRACTED@@ */", funcs)):
        idx = fx.index(marker)
        out += fx[pos:idx] + "\n" + text
        out += '#line %d "%s"\n' % (ub01.line_of(fx, idx) + 1, FIXTURE)
        pos = idx + len(marker)
    return out + fx[pos:]


def run_config(work, name, defines, get, label, fixed, verbose):
    types, funcs = extract(get, label, fixed)
    if not fixed:
        defines = defines + ["BASELINE"]
    exe = ub01.build(work, name, generate(types, funcs, defines))
    r, res = ub01.run(exe, verbose)
    return r, res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true",
                    help="keep the generated sources and binaries")
    ap.add_argument("--verbose", action="store_true",
                    help="print U-Boot messages from the fixture")
    args = ap.parse_args()

    work = Path(tempfile.mkdtemp(prefix="ub02-upgrade-"))
    ok = True
    try:
        for cfg, defines in CONFIGS.items():
            r, res = run_config(work, "fixed_" + cfg, defines,
                                ub01.read_tree, lambda p: str(ROOT / p),
                                True, args.verbose)
            sys.stdout.write(r.stderr)
            passed = sum(v == "PASS" for v in res.values())
            if r.returncode != 0 or not res or passed != len(res):
                ok = False
                if not args.verbose:
                    sys.stdout.write(r.stdout[-4000:])
            print("fixed %s: %d/%d scenarios passed" % (cfg, passed, len(res)))

        for name, path, old, new, cfg, expected in MUTATIONS:
            def get(p, path=path, old=old, new=new):
                text = ub01.read_tree(p)
                if p == path:
                    if text.count(old) != 1:
                        raise SystemExit("mutation %s: anchor not found"
                                         % name)
                    text = text.replace(old, new)
                return text
            r, res = run_config(work, "mut_%s_%s" % (name, cfg),
                                CONFIGS[cfg], get, lambda p: str(ROOT / p),
                                True, False)
            caught = [t for t in expected if res.get(t) == "FAIL"]
            print("mutation %-12s %-6s %d/%d expected failures: %s"
                  % (name, cfg, len(caught), len(expected),
                     ", ".join(expected)))
            if len(caught) != len(expected):
                ok = False

        if ub01.read_rev(BASELINE_REV, FLASHWRITE) is None:
            print("note: baseline %s not available; defect detection "
                  "skipped" % BASELINE_REV[:12])
        else:
            get = lambda p: ub01.read_rev(BASELINE_REV, p)
            for cfg, defines in CONFIGS.items():
                r, res = run_config(work, "baseline_" + cfg, defines, get,
                                    lambda p: "baseline:" + p, False, False)
                expected = BASELINE_DEFECTS[cfg]
                caught = [t for t in expected if res.get(t) == "FAIL"]
                for t in expected:
                    print("baseline %-6s %-42s %s" % (
                        cfg, t, "defect detected" if t in caught
                        else "NOT DETECTED"))
                unexpected = [t for t, v in res.items()
                              if v == "FAIL" and t not in expected]
                for t in unexpected:
                    print("baseline %-6s %-42s also fails" % (cfg, t))
                if len(caught) != len(expected):
                    ok = False
                print("baseline %s %s: %d/%d defects detected"
                      % (BASELINE_REV[:12], cfg, len(caught), len(expected)))
    finally:
        if args.keep:
            print("build dir:", work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    print("RESULT:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
