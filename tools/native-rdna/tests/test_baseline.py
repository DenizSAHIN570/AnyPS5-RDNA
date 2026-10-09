import contextlib
import io
import json
import os
import stat
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import baseline

SHEBANG = "#!" + sys.executable + "\n"

FAKE_RELINKER = textwrap.dedent("""\
    import sys
    from pathlib import Path
    args = sys.argv[1:]
    assert args[0] == "--registry", args
    output = Path(args[-1])
    output.write_text("#!/bin/sh\\nexit 0\\n")
    module = output.parent / "app0" / "sce_module"
    module.mkdir(parents=True, exist_ok=True)
    (module / "libc.prx.guest.prx").write_text("guest")
    print("Guest module:", module / "libc.prx.guest.prx")
    """)

FAKE_TITLE = textwrap.dedent("""\
    import os, sys, time
    with open("frame-timing.log", "w") as log:
        for i in range(200):
            interval = 16.667 if i % 50 else 50.0
            log.write(f"[FrameTiming] frame={i} flip_complete_at_ms={i * 16.667:.3f} flip_interval_ms={interval:.3f} gpu_busy_ms=4.000 worker_span_ms=10.000 packet_count=120 metrics=inclusive(count,sum_ms,max_ms[,bytes]) Driver.Packet.total=(5,1.0,0.5)\\n")
    print("[guestmem] reads 1", file=sys.stderr)
    print("[recorder] waits 2", file=sys.stderr)
    print("profile", os.environ.get("APS5_PROFILE_DRAW"), "cwd-app0", os.path.isdir("app0"))
    if os.environ.get("APS5_DUMP_REGISTERED_SHADERS"):
        os.makedirs(os.environ["APS5_DUMP_REGISTERED_SHADERS"], exist_ok=True)
        open(os.path.join(os.environ["APS5_DUMP_REGISTERED_SHADERS"], "shader_1_t0_0.ash"), "wb").close()
    if os.environ.get("APS5_MEMORY_CENSUS"):
        with open(os.environ["APS5_MEMORY_CENSUS"], "w") as census:
            census.write("seconds,op\\n0.1,allocate\\n0.2,map_direct\\n")
    if os.environ.get("HANG") == "1":
        time.sleep(60)
    """)


def executable(path, text):
    path.write_text(SHEBANG + text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


@unittest.skipUnless(sys.platform == "linux", "prepare and run are Linux-only")
class BaselineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.relinker = self.root / "relinker"
        executable(self.relinker, FAKE_RELINKER)
        (self.root / "libs").mkdir()
        for name in ("libkernel.prx", "libc.prx"):
            (self.root / "libs" / name).write_text(name)
        self.app0 = self.root / "game" / "app0"
        (self.app0 / "sce_module").mkdir(parents=True)
        (self.app0 / "data").mkdir()
        (self.app0 / "data" / "level.bin").write_text("data")
        (self.app0 / "eboot.elf").write_text("elf")
        self.out = self.root / "out"

    def tearDown(self):
        self.tmp.cleanup()

    def invoke(self, *argv):
        with patch("sys.argv", ["baseline.py", *map(str, argv)]), contextlib.redirect_stdout(io.StringIO()) as printed:
            code = baseline.main()
        return code, printed.getvalue()

    def prepare(self):
        return self.invoke("prepare", "--relinker", self.relinker, "--input", self.app0 / "eboot.elf", "--libs", self.root / "libs", "--app0", self.app0, "--out", self.out)

    def test_prepare_builds_the_layout(self):
        code, _ = self.prepare()
        self.assertEqual(code, 0)
        self.assertTrue((self.out / "app.elf").exists())
        self.assertTrue((self.out / "libs" / "libkernel.prx").exists())
        self.assertTrue((self.out / "app0" / "sce_module" / "libc.prx.guest.prx").exists())
        self.assertFalse((self.out / "app0" / "sce_module").is_symlink())
        self.assertTrue((self.out / "app0" / "data").is_symlink())
        manifest = json.loads((self.out / "prepare.json").read_text())
        self.assertEqual(manifest["libraries"], 2)
        self.assertEqual(manifest["linkedResources"], 2)

    def test_prepare_refuses_a_used_directory(self):
        self.out.mkdir()
        (self.out / "keep").write_text("x")
        with self.assertRaises(SystemExit):
            self.prepare()

    def test_run_collects_and_summarizes(self):
        self.prepare()
        executable(self.out / "app.elf", FAKE_TITLE)
        code, printed = self.invoke("run", "--out", self.out, "--seconds", "20", "--label", "m0", "--dump-shaders", "--memory-census", "--env", "RADV_DEBUG=nocache")
        self.assertEqual(code, 0)
        run_dir = self.out / "runs" / "m0"
        metadata = json.loads((run_dir / "metadata.json").read_text())
        self.assertEqual(metadata["ending"], "exited")
        self.assertEqual(metadata["exitCode"], 0)
        self.assertEqual(metadata["environment"]["APS5_PROFILE_DRAW"], "1")
        self.assertEqual(metadata["environment"]["RADV_DEBUG"], "nocache")
        self.assertIn("profile 1 cwd-app0 True", (run_dir / "stdout.log").read_text())
        summary = json.loads((run_dir / "summary.json").read_text())
        self.assertEqual(summary["frames"], 200)
        self.assertAlmostEqual(summary["gpu_busy_ms"]["mean"], 4.0)
        self.assertEqual(summary["dumpedShaders"], 1)
        self.assertEqual(summary["memoryCensusRows"], 2)
        self.assertEqual(summary["profileLines"]["guestmem"], 1)
        self.assertGreater(summary["fps"]["average"], summary["fps"]["onePercentLow"])
        self.assertIn("fps 1% low", printed)

    def test_run_stops_a_title_that_keeps_running(self):
        self.prepare()
        executable(self.out / "app.elf", FAKE_TITLE)
        code, _ = self.invoke("run", "--out", self.out, "--seconds", "2", "--grace", "1", "--label", "hang", "--env", "HANG=1")
        self.assertEqual(code, 0)
        metadata = json.loads((self.out / "runs" / "hang" / "metadata.json").read_text())
        self.assertIn(metadata["ending"], ("interrupted", "killed"))

    def test_summarize_skips_warmup(self):
        self.prepare()
        executable(self.out / "app.elf", FAKE_TITLE)
        self.invoke("run", "--out", self.out, "--seconds", "20", "--label", "warm")
        code, _ = self.invoke("summarize", self.out / "runs" / "warm", "--skip-seconds", "1")
        self.assertEqual(code, 0)
        summary = json.loads((self.out / "runs" / "warm" / "summary.json").read_text())
        self.assertLess(summary["frames"], 200)


class SummaryTests(unittest.TestCase):
    def test_percentile_and_records(self):
        self.assertEqual(baseline.percentile([5, 1, 3], 0.5), 3)
        self.assertIsNone(baseline.percentile([], 0.5))
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "frame-timing.log"
            log.write_text("noise\n[FrameTiming] frame=1 flip_interval_ms=20.000 Driver.Packet.total=(1,2.0,3.0)\n")
            records = baseline.frame_records(log)
            self.assertEqual(records, [{"frame": 1.0, "flip_interval_ms": 20.0}])


if __name__ == "__main__":
    unittest.main()
