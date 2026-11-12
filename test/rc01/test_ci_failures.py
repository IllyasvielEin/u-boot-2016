#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise real CI entry points with failing build tools and page inputs."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TARGET = "ipq9574_xiaomi_be10000_defconfig"
OUTPUT = "bin/openwrt-ipq9574_xiaomi_be10000-u-boot.mbn"


def executable(path, source):
    path.write_text(source)
    path.chmod(0o755)


class BuildFailures(unittest.TestCase):
    def test_real_build_driver(self):
        for stage in ("ok", "clean", "configure", "compile", "strip", "pack", "empty"):
            with self.subTest(stage=stage), tempfile.TemporaryDirectory(prefix="rc01-driver-") as tmp:
                work = Path(tmp)
                (work / "configs").mkdir()
                (work / "configs" / TARGET).touch()
                (work / "tools").mkdir()
                (work / "mockbin").mkdir()
                shutil.copyfile(ROOT / "build.sh", work / "build.sh")
                executable(work / "mockbin/make", """#!/bin/sh
case "$*" in
  *clean*) [ "$FAIL_STAGE" != clean ] ;;
  *defconfig*) [ "$FAIL_STAGE" != configure ] ;;
  *) [ "$FAIL_STAGE" != compile ] || exit 23; printf ELF > u-boot ;;
esac
""")
                executable(work / "mockbin/arm-openwrt-linux-strip", """#!/bin/sh
cp "$1" "$3" || exit
[ "$FAIL_STAGE" != strip ]
""")
                (work / "tools/elftombn_py3.py").write_text("""import os, pathlib, sys
p = pathlib.Path(sys.argv[sys.argv.index('-o') + 1])
p.write_bytes(b'' if os.environ['FAIL_STAGE'] == 'empty' else b'packaged')
raise SystemExit(23 if os.environ['FAIL_STAGE'] == 'pack' else 0)
""")
                result = subprocess.run(["bash", "build.sh", TARGET], cwd=work,
                                        env=dict(os.environ, FAIL_STAGE=stage,
                                                 PATH=str(work / "mockbin") + ":" + os.environ["PATH"]),
                                        capture_output=True, text=True, timeout=30)
                if stage == "ok":
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual((work / OUTPUT).read_bytes(), b"packaged")
                else:
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertFalse((work / OUTPUT).exists(), result.stdout)

    def test_web_generator_failure_reaches_make(self):
        recipe = next(line for line in (ROOT / "Makefile").read_text().splitlines()
                      if "./html/makefsdatac" in line)
        with tempfile.TemporaryDirectory(prefix="rc01-web-failure-") as tmp:
            work = Path(tmp)
            (work / "failsafe/html").mkdir(parents=True)
            shutil.copy2(ROOT / "failsafe/html/makefsdatac", work / "failsafe/html/makefsdatac")
            (work / "failsafe/fsdata.c").write_text("previous resources\n")
            (work / "mockbin").mkdir()
            executable(work / "mockbin/node", "#!/bin/sh\nexit 23\n")
            (work / "Makefile").write_text("srctree := .\nall:\n" + recipe + "\n")
            result = subprocess.run(["make"], cwd=work,
                                    env=dict(os.environ, PATH=str(work / "mockbin") + ":" + os.environ["PATH"]),
                                    capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_invalid_generated_page_fails_node(self):
        with tempfile.TemporaryDirectory(prefix="rc01-page-failure-") as tmp:
            empty = Path(tmp) / "about.html"
            empty.write_text("")
            result = subprocess.run(["node", str(ROOT / "test/rc01/env_web_fixture.js"),
                                     str(ROOT / "failsafe/html/pig/common.js"), str(empty),
                                     str(ROOT / "failsafe/html/pig/term.html")],
                                    capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("FAIL about_", result.stderr)

    def test_missing_baseline_fails_strict_mode(self):
        result = subprocess.run([sys.executable, "-B", "-c",
                                 "import test_nand_write_safety as t; "
                                 "t.read_rev('0000000000000000000000000000000000000000', 'Makefile')"],
                                cwd=ROOT / "test/rc01", env=dict(os.environ, RC01_CI_STRICT="1"),
                                capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required regression baseline unavailable", result.stderr)

    def test_sanitizer_failure_has_no_strict_fallback(self):
        with tempfile.TemporaryDirectory(prefix="rc01-cc-failure-") as tmp:
            work = Path(tmp)
            cc = work / "cc"
            executable(cc, '#!/bin/sh\nprintf "call\\n" >> "$CC_CALL_LOG"\nexit 23\n')
            calls = work / "calls"
            result = subprocess.run([sys.executable, "-B", "-c",
                                     "from pathlib import Path; import sys; "
                                     "import test_nand_write_safety as t; "
                                     "t.build(Path(sys.argv[1]), 'failure', 'int main(void) { return 0; }')",
                                     str(work)], cwd=ROOT / "test/rc01",
                                    env=dict(os.environ, RC01_CI_STRICT="1", CC=str(cc), CC_CALL_LOG=str(calls)),
                                    capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("sanitizer build failed", result.stderr)
            self.assertEqual(calls.read_text().splitlines(), ["call"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
