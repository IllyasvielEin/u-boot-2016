#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline candidate check for the Xiaomi BE10000 (ipq9574) U-Boot MBN.

Checks one candidate produced by ./build.sh ipq9574_xiaomi_be10000_defconfig
against the outputs of the same in-tree build:

  * the final v6 MBN: ELF32 ARM container, segment/EOF bounds, the Qualcomm
    v6 header and SHA-384 hash table, LOAD/entry identical to the built ELF;
  * the one embedded compressed DTB is BE10000 (model, machid, C4 config
    names, four QCA8084 ports);
  * .config / u-boot.cfg select BE10000 on ARM32 with a 64 KiB
    non-redundant NAND ENV;
  * APPSBL budget (1572864 bytes, page/erase-block rounded; the real bad
    block count is unknown here and is NOT accounted for);
  * failsafe/fsdata.c of this build: resource list, HTTP headers, complete
    gzip bodies, every response present in the final LOAD payload, and the
    generated common.js/about.html/term.html pass env_web_fixture.js.

Usage:
  check_be10000_candidate.py --tree . --mbn bin/openwrt-ipq9574_xiaomi_be10000-u-boot.mbn \\
      --result out/check-result.json [--candidate-dir out/candidate] [--meta KEY=VALUE ...]

The candidate directory (MBN, manifest.json, SHA256SUMS, check-result.json)
is written only if every check passes. Exit status: 0 pass, 1 rejected,
2 usage error. Standard library only; needs fdtget and node on PATH.
"""

import argparse
import hashlib
import json
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_FIXTURE = HERE.parent.parent / "test/rc01/env_web_fixture.js"

# Target facts (arch/arm/dts/ipq9574-xiaomi-be10000.dts, configs/, and the
# RC01 layout immortalwrt/config/rc01/layout.json in the development copy).
MODEL = "Xiaomi BE10000"
MACHID = "8050301"
CONFIG_NAMES = ["config@al02-c4", "config-al02-c4", "config@rdp433", "config-rdp433"]
SWITCH_MAC_MODE0 = "15"            # PORT_WRAPPER_UQXGMII
PORTS = {0: ("6", "1"), 1: ("6", "2"), 2: ("6", "3"), 3: ("6", "4")}   # QCA8084
UNUSED_PORTS = (4, 5)               # phy_type 0xff
APPSBL_BYTES = 1572864
NAND_PAGE = 2048
NAND_ERASE = 131072
ENV_BYTES = 64 * 1024
WEB_SCENARIOS = 37

ELF_HDR = struct.Struct("<16sHHIIIIIHHHHHH")
PHDR = struct.Struct("<8I")
PT_NULL, PT_LOAD = 0, 1
EM_ARM = 40
HASH_SEG_FLAGS = 0x02200000
DIGEST = 48                         # SHA-384
MBN_V6_HDR = 48                     # 12 little-endian words
FDT_MAGIC = b"\xd0\x0d\xfe\xed"


class Reject(Exception):
    pass


def need(cond, msg):
    if not cond:
        raise Reject(msg)


# ---------------------------------------------------------------- ELF

def parse_elf(data, label):
    need(len(data) >= ELF_HDR.size, "%s: %d bytes, too short for an ELF header" % (label, len(data)))
    h = ELF_HDR.unpack_from(data)
    need(h[0][:4] == b"\x7fELF", "%s: not an ELF file" % label)
    need(h[0][4] == 1 and h[0][5] == 1, "%s: not ELF32 little-endian" % label)
    need(h[2] == EM_ARM, "%s: e_machine %d is not ARM" % (label, h[2]))
    need(h[8] == ELF_HDR.size and h[9] == PHDR.size, "%s: unexpected header sizes" % label)
    phoff, phnum = h[5], h[10]
    need(phnum > 0 and phoff + phnum * PHDR.size <= len(data),
         "%s: program header table beyond EOF" % label)
    ph = [PHDR.unpack_from(data, phoff + i * PHDR.size) for i in range(phnum)]
    for i, p in enumerate(ph):
        need(p[1] + p[4] <= len(data),
             "%s: segment %d [0x%x+0x%x] beyond EOF 0x%x" % (label, i, p[1], p[4], len(data)))
    return {"e_type": h[1], "entry": h[4],
            "phoff": phoff, "phnum": phnum, "ph": ph}


def seg(data, p):
    return data[p[1]:p[1] + p[4]]


def check_mbn(mbn, elf, text_base, res):
    need(len(mbn) > 0, "candidate MBN is empty")
    m = parse_elf(mbn, "MBN")
    e = parse_elf(elf, "ELF")
    end = max(p[1] + p[4] for p in m["ph"])
    need(end == len(mbn), "MBN size %d != last segment end %d (truncated or trailing data)"
         % (len(mbn), end))

    loads = [p for p in m["ph"] if p[0] == PT_LOAD]
    eloads = [p for p in e["ph"] if p[0] == PT_LOAD]
    need(len(loads) == 1 and len(eloads) == 1, "expected exactly one PT_LOAD in ELF and MBN")
    load, eload = loads[0], eloads[0]
    need(load[2:] == eload[2:], "MBN LOAD header differs from the built ELF")
    need(seg(mbn, load) == seg(elf, eload), "MBN LOAD payload differs from the built ELF")
    need(m["entry"] == e["entry"] == load[2] == text_base,
         "entry 0x%x / ELF 0x%x / LOAD 0x%x / TEXT_BASE 0x%x disagree"
         % (m["entry"], e["entry"], load[2], text_base))

    # Packer layout: [0] NULL ELF+PHDR segment, [1] hash segment, then the
    # input program headers in order (offsets shifted).
    p0, p1 = m["ph"][0], m["ph"][1]
    need(p0[0] == PT_NULL and p0[1] == 0 and p0[4] == m["phoff"] + m["phnum"] * PHDR.size,
         "MBN segment 0 is not the ELF header + program headers")
    need(p1[6] == HASH_SEG_FLAGS, "MBN segment 1 is not a hash segment")
    need(m["phnum"] == e["phnum"] + 2, "MBN has %d program headers, ELF %d"
         % (m["phnum"], e["phnum"]))
    for a, b in zip(m["ph"][2:], e["ph"]):
        need(a[0] == b[0] and a[2:] == b[2:], "MBN program headers do not mirror the ELF")

    hs = seg(mbn, p1)
    table = m["phnum"] * DIGEST
    need(len(hs) == MBN_V6_HDR + table, "hash segment %d bytes, expected %d"
         % (len(hs), MBN_V6_HDR + table))
    hdr = struct.unpack_from("<12I", hs)
    need(hdr[1] == 6, "MBN header version %d, expected 6" % hdr[1])
    need(hdr[4] == hdr[5] == table, "MBN image/code size %d/%d, expected %d"
         % (hdr[4], hdr[5], table))
    need(not any(hdr[2:4]) and not any(hdr[6:12]),
         "unexpected signature/certificate/metadata fields in an unsigned v6 header")
    want = [hashlib.sha384(seg(mbn, p0)).digest(), bytes(DIGEST)]
    for p in m["ph"][2:]:
        want.append(hashlib.sha384(seg(mbn, p)).digest() if p[4] else bytes(DIGEST))
    got = hs[MBN_V6_HDR:]
    for i, w in enumerate(want):
        need(got[i * DIGEST:(i + 1) * DIGEST] == w, "SHA-384 hash entry %d mismatch" % i)

    res["mbn"] = {"bytes": len(mbn), "sha256": hashlib.sha256(mbn).hexdigest(),
                  "program_headers": m["phnum"], "entry": hex(m["entry"]),
                  "load": {"offset": load[1], "vaddr": hex(load[2]), "filesz": load[4],
                           "memsz": load[5], "sha384": hashlib.sha384(seg(mbn, load)).hexdigest()},
                  "v6_header": list(hdr)}
    res["elf"] = {"bytes": len(elf), "sha256": hashlib.sha256(elf).hexdigest(),
                  "e_type": e["e_type"], "entry": hex(e["entry"])}
    return seg(mbn, load)


# ---------------------------------------------------------------- config

def parse_cfg(text):
    defs = {}
    for line in text.splitlines():
        mt = re.match(r"#define\s+(\w+)(?:\s+(.*))?$", line)
        if mt:
            defs[mt.group(1)] = (mt.group(2) or "").strip()
    return defs


def eval_macro(defs, name, depth=0):
    need(depth < 16 and name in defs, "macro %s is not defined" % name)
    expr = re.sub(r"0[xX][0-9a-fA-F]+|\d+|[A-Za-z_]\w*",
                  lambda mm: mm.group(0) if mm.group(0)[0].isdigit()
                  else "(%d)" % eval_macro(defs, mm.group(0), depth + 1),
                  defs[name])
    need(re.fullmatch(r"[0-9a-fA-FxX()+\-*<> \t]*", expr) and expr.strip(),
         "macro %s is not a plain integer expression" % name)
    return int(eval(expr, {"__builtins__": {}}))   # digits/operators only


def check_config(tree, res):
    cfg = (tree / ".config").read_text()
    on = set(re.findall(r"^(CONFIG_\w+)=y$", cfg, re.M))
    need("CONFIG_ARM" in on and "CONFIG_ARM64" not in on, ".config is not ARM32")
    need("CONFIG_IPQ9574_XIAOMI_BE10000" in on, ".config does not select BE10000")
    other = sorted(c for c in on if re.match(r"CONFIG_IPQ\w*_XIAOMI_", c)
                   and c != "CONFIG_IPQ9574_XIAOMI_BE10000")
    need(not other, ".config also selects %s" % other)
    need('CONFIG_SYS_BOARD="ipq9574"' in cfg, ".config SYS_BOARD is not ipq9574")

    defs = parse_cfg((tree / "u-boot.cfg").read_text())
    need("CONFIG_IPQ9574_XIAOMI_BE10000" in defs, "u-boot.cfg lacks BE10000")
    need("CONFIG_ENV_IS_IN_NAND" in defs, "u-boot.cfg: ENV is not in NAND")
    env = eval_macro(defs, "CONFIG_ENV_SIZE")
    need(env == ENV_BYTES, "CONFIG_ENV_SIZE %d, expected %d" % (env, ENV_BYTES))
    redund = [d for d in ("CONFIG_SYS_REDUNDAND_ENVIRONMENT", "CONFIG_ENV_OFFSET_REDUND",
                          "CONFIG_ENV_ADDR_REDUND") if d in defs]
    need(not redund, "redundant ENV enabled: %s" % redund)
    need("CONFIG_LWIP_HTTPD" in defs, "u-boot.cfg lacks CONFIG_LWIP_HTTPD")
    text_base = eval_macro(defs, "CONFIG_SYS_TEXT_BASE")
    res["config"] = {"board": "ipq9574_xiaomi_be10000", "env_bytes": env,
                     "env_redundant": False, "text_base": hex(text_base)}
    return text_base


# ---------------------------------------------------------------- DTB

def gzip_members(payload):
    """Yield (offset, compressed bytes, data) for each complete gzip stream."""
    pos = 0
    while True:
        pos = payload.find(b"\x1f\x8b\x08", pos)
        if pos < 0:
            return
        d = zlib.decompressobj(31)
        chunk = payload[pos:]
        try:
            out = d.decompress(chunk)
            if d.eof:
                yield pos, chunk[:len(chunk) - len(d.unused_data)], out
        except zlib.error:
            pass
        pos += 1


def fdts(blob):
    found, pos = [], 0
    while True:
        pos = blob.find(FDT_MAGIC, pos)
        if pos < 0 or pos + 40 > len(blob):
            return found
        total, ostruct, ostr, _, ver = struct.unpack_from(">5I", blob, pos + 4)
        if 40 <= total <= len(blob) - pos and ostruct < total and ostr < total and ver >= 16:
            found.append(blob[pos:pos + total])
            pos += total
        else:
            pos += 1


def fdtget(path, node, prop, typ):
    r = subprocess.run(["fdtget", "-t", typ, str(path), node, prop],
                       capture_output=True, text=True)
    need(r.returncode == 0, "fdtget %s %s: %s" % (node, prop, r.stderr.strip()))
    return r.stdout.strip()


def check_dtb(payload, tree, work, res):
    blobs = [(off, comp, raw) for off, comp, raw in gzip_members(payload) if fdts(raw)]
    need(len(blobs) == 1, "expected one compressed DTB blob in LOAD, found %d" % len(blobs))
    off, comp, raw = blobs[0]
    trees = fdts(raw)
    need(len(trees) == 1, "compressed DTB blob holds %d device trees, expected 1" % len(trees))
    built = tree / "dtb_combined.bin.gz"
    if built.exists():
        need(built.read_bytes() == comp, "embedded DTB differs from this build's dtb_combined.bin.gz")
    dtb = work / "embedded.dtb"
    dtb.write_bytes(trees[0])
    need(fdtget(dtb, "/", "model", "s") == MODEL, "DTB model is not %s" % MODEL)
    need(fdtget(dtb, "/", "machid", "x") == MACHID, "DTB machid is not 0x%s" % MACHID)
    need(fdtget(dtb, "/", "config_name", "s").split() == CONFIG_NAMES,
         "DTB config_name is not the C4 list")
    need(fdtget(dtb, "/ess-switch", "switch_mac_mode0", "x") == SWITCH_MAC_MODE0,
         "switch_mac_mode0 is not UQXGMII")
    base = "/ess-switch/port_phyinfo/port@%d"
    for i, (typ, addr) in PORTS.items():
        need(fdtget(dtb, base % i, "phy_type", "x") == typ, "port %d is not QCA8084" % i)
        need(fdtget(dtb, base % i, "phy_address", "x") == addr, "port %d phy_address" % i)
    for i in UNUSED_PORTS:
        need(fdtget(dtb, base % i, "phy_type", "x") == "ff", "port %d is not unused" % i)
    res["dtb"] = {"load_offset": off, "gzip_bytes": len(comp), "fdt_bytes": len(trees[0]),
                  "sha256": hashlib.sha256(trees[0]).hexdigest(), "model": MODEL,
                  "machid": "0x" + MACHID, "lan_ports": 4}


# ---------------------------------------------------------------- web

def parse_fsdata(text):
    arrays = {}
    for name, body in re.findall(r"static const char data_(\w+)\[\] = \{([^}]*)\};", text):
        b = bytes(int(x, 16) for x in body.split(","))
        need(b.endswith(b"\0"), "fsdata %s is not NUL terminated" % name)
        arrays[name] = b[:-1]
    files = dict((path, arrays.get(name)) for path, name in
                 re.findall(r'"(/[^"]+)",data_(\w+),', text))
    num = re.search(r"#define FS_NUMFILES (\d+)", text)
    need(files and num and int(num.group(1)) == len(files) == len(arrays),
         "fsdata.c file table is inconsistent")
    need(all(v is not None for v in files.values()), "fsdata.c references a missing array")
    return files


def split_response(path, resp):
    head, sep, body = resp.partition(b"\r\n\r\n")
    need(sep, "%s: no HTTP header terminator" % path)
    lines = head.decode("ascii").split("\r\n")
    status = "HTTP/1.0 404 File not found" if path == "/404.html" else "HTTP/1.0 200 OK"
    need(lines[0] == status, "%s: status line %r" % (path, lines[0]))
    hdrs = dict(l.split(": ", 1) for l in lines[1:])
    ctype = {k.lower(): v for k, v in hdrs.items()}.get("content-type", "")
    need(ctype, "%s: no Content-Type" % path)
    if hdrs.get("Content-Encoding") == "gzip":
        d = zlib.decompressobj(31)
        try:
            out = d.decompress(body)
        except zlib.error as e:
            raise Reject("%s: gzip body does not decompress: %s" % (path, e))
        need(d.eof and not d.unused_data, "%s: gzip body incomplete or has trailing data" % path)
        body = out
    else:
        need(not re.search(r"\.(html|js|css|svg)$", path), "%s: text resource not gzip" % path)
    need(body, "%s: empty body" % path)
    return ctype, body


def run_web_fixture(fixture, pages):
    cmd = ["node", str(fixture)] + [str(pages[n]) for n in ("common.js", "about.html", "term.html")]
    lst = subprocess.run(cmd + ["--list"], capture_output=True, text=True, timeout=120)
    names = [l.strip() for l in lst.stderr.splitlines() if l.strip()]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    passed = [l[5:].strip() for l in r.stderr.splitlines() if l.startswith("PASS ")]
    failed = [l[5:].strip() for l in r.stderr.splitlines() if l.startswith("FAIL ")]
    out = {"exit": r.returncode, "listed": len(names), "pass": len(passed), "fail": failed}
    if failed or r.returncode or lst.returncode:
        out["log"] = (r.stdout + r.stderr)[-4000:]
    return out, names, passed


def check_web(tree, payload, fixture, work, res):
    files = parse_fsdata((tree / "failsafe/fsdata.c").read_text())
    src = tree / "failsafe/html/pig"
    expect = sorted("/" + p.name for p in src.iterdir() if p.is_file())
    need(sorted(files) == expect, "fsdata resources %s != html/pig %s" % (sorted(files), expect))
    pages, manifest = {}, {}
    for path, resp in sorted(files.items()):
        need(payload.find(resp) >= 0, "%s: generated response is not in the final LOAD" % path)
        ctype, body = split_response(path, resp)
        manifest[path] = {"response_bytes": len(resp), "body_bytes": len(body),
                          "content_type": ctype, "sha256": hashlib.sha256(body).hexdigest(),
                          "differs_from_source": body != (src / path[1:]).read_bytes()}
        name = path[1:]
        if name in ("common.js", "about.html", "term.html"):
            pages[name] = work / name
            pages[name].write_bytes(body)
    need(len(pages) == 3, "common.js/about.html/term.html missing from fsdata")
    fx, names, passed = run_web_fixture(fixture, pages)
    res["web"] = {"resources": manifest, "fixture": fx}
    need(fx["exit"] == 0 and not fx["fail"], "generated pages: fixture exit %d, %d FAIL"
         % (fx["exit"], len(fx["fail"])))
    need(len(names) == WEB_SCENARIOS and sorted(passed) == sorted(names),
         "generated pages: %d/%d scenarios passed, expected %d"
         % (len(passed), len(names), WEB_SCENARIOS))


# ---------------------------------------------------------------- budget

def budget(size):
    pages = -(-size // NAND_PAGE)
    blocks = -(-size // NAND_ERASE)
    total = APPSBL_BYTES // NAND_ERASE
    need(blocks <= total, "candidate needs %d erase blocks, APPSBL has %d" % (blocks, total))
    return {"appsbl_bytes": APPSBL_BYTES, "page_bytes": NAND_PAGE, "erase_bytes": NAND_ERASE,
            "pages": pages, "page_rounded_bytes": pages * NAND_PAGE,
            "erase_blocks": blocks, "appsbl_blocks": total, "spare_blocks": total - blocks,
            "note": "nominal; real bad blocks unknown, runtime precheck still required"}


# ---------------------------------------------------------------- main

def tool_version(cmd):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return (r.stdout or r.stderr).strip().splitlines()[0]
    except (OSError, IndexError, subprocess.SubprocessError):
        return None


def check(tree, mbn_path, fixture):
    res = {"tree": str(tree), "candidate": mbn_path.name, "checks": {}}
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        steps = [("config", lambda: res.__setitem__("_tb", check_config(tree, res))),
                 ("mbn", lambda: res.__setitem__("_load", check_mbn(
                     mbn_path.read_bytes(), (tree / "u-boot").read_bytes(), res["_tb"], res))),
                 ("budget", lambda: res.__setitem__("budget", budget(mbn_path.stat().st_size))),
                 ("dtb", lambda: check_dtb(res["_load"], tree, work, res)),
                 ("web", lambda: check_web(tree, res["_load"], fixture, work, res))]
        for name, fn in steps:
            try:
                fn()
                res["checks"][name] = "PASS"
            except (Reject, OSError, ValueError, UnicodeDecodeError, KeyError) as e:
                res["checks"][name] = "FAIL: %s" % e
                break
    res.pop("_tb", None)
    res.pop("_load", None)
    res["ok"] = len(res["checks"]) == len(steps) and all(
        v == "PASS" for v in res["checks"].values())
    return res


def write_candidate(out, mbn_path, res, meta):
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(mbn_path, out / mbn_path.name)
    manifest = dict(meta)
    manifest.update({
        "candidate": {"file": mbn_path.name, "bytes": res["mbn"]["bytes"],
                      "sha256": res["mbn"]["sha256"]},
        "elf": res["elf"], "dtb": res["dtb"], "config": res["config"], "budget": res["budget"],
        "web_resources": {k: {"sha256": v["sha256"], "body_bytes": v["body_bytes"]}
                          for k, v in res["web"]["resources"].items()},
        "web_fixture": {"pass": res["web"]["fixture"]["pass"], "fail": 0},
        "tools": {"python": platform.python_version(), "node": tool_version(["node", "--version"]),
                  "dtc": tool_version(["dtc", "--version"])},
        "checks": res["checks"],
        "scope": "static build candidate; not a device boot, flash or release qualification",
    })
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    (out / "check-result.json").write_text(json.dumps(res, indent=2, sort_keys=True) + "\n")
    sums = "".join("%s  %s\n" % (hashlib.sha256((out / n).read_bytes()).hexdigest(), n)
                   for n in (mbn_path.name, "manifest.json", "check-result.json"))
    (out / "SHA256SUMS").write_text(sums)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tree", type=Path, default=Path("."), help="built U-Boot source tree")
    ap.add_argument("--mbn", type=Path, required=True, help="candidate MBN from this build")
    ap.add_argument("--result", type=Path, required=True, help="JSON result, always written")
    ap.add_argument("--candidate-dir", type=Path, help="written only when all checks pass")
    ap.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE)
    ap.add_argument("--meta", action="append", default=[], metavar="KEY=VALUE",
                    help="manifest field (source SHA, toolchain commit, profile, ...)")
    a = ap.parse_args(argv)
    meta = {}
    for kv in a.meta:
        k, sep, v = kv.partition("=")
        if not sep or not k:
            ap.error("--meta expects KEY=VALUE: %r" % kv)
        meta[k] = v
    for tool in ("fdtget", "node"):
        if not shutil.which(tool):
            ap.error("%s is required" % tool)

    tree = a.tree.resolve()
    if a.candidate_dir and a.candidate_dir.exists():
        shutil.rmtree(a.candidate_dir)
    res = check(tree, a.mbn, a.fixture)
    a.result.parent.mkdir(parents=True, exist_ok=True)
    a.result.write_text(json.dumps(res, indent=2, sort_keys=True) + "\n")
    for k, v in res["checks"].items():
        print("%-7s %s" % (k, v))
    if not res["ok"]:
        print("RESULT: REJECTED")
        return 1
    if a.candidate_dir:
        write_candidate(a.candidate_dir, a.mbn, res, meta)
    print("RESULT: OK %s %d bytes sha256 %s" % (a.mbn.name, res["mbn"]["bytes"],
                                                res["mbn"]["sha256"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
