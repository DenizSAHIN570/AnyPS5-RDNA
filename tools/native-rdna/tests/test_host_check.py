import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import host_check


def fake_host(root, visible_gib, vendor="0x1002"):
    files = {
        "etc/os-release": 'NAME="Test"\nPRETTY_NAME="Test Linux 1.0"\n',
        "proc/cpuinfo": "processor\t: 0\nmodel name\t: Test CPU 9000\n",
        "proc/sys/kernel/osrelease": "6.99.0-test\n",
        "sys/class/drm/card1/device/vendor": vendor + "\n",
        "sys/class/drm/card1/device/device": "0x7550\n",
        "sys/class/drm/card1/device/uevent": "DRIVER=amdgpu\nPCI_SLOT_NAME=0000:03:00.0\n",
        "sys/class/drm/card1/device/mem_info_vram_total": str(16 * 1024 ** 3) + "\n",
        "sys/class/drm/card1/device/mem_info_vis_vram_total": str(visible_gib * 1024 ** 3) + "\n",
        "sys/class/drm/card1/device/mem_info_gtt_total": str(32 * 1024 ** 3) + "\n",
        "sys/class/drm/card1/device/ip_discovery/die/0/GC/0/major": "12\n",
        "sys/class/drm/card1/device/ip_discovery/die/0/GC/0/minor": "0\n",
        "sys/class/drm/card1/device/ip_discovery/die/0/GC/0/revision": "1\n",
        "sys/class/drm/card1-DP-1/status": "connected\n",
        "sys/module/udmabuf/parameters/size_limit_mb": "64\n",
        "sys/module/udmabuf/parameters/list_limit": "1024\n",
        "sys/module/ttm/parameters/pages_limit": "3407872\n",
    }
    for relative, content in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)


class HostCheckTests(unittest.TestCase):
    def test_full_resizable_bar_rdna4(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_host(root, 16)
            report = host_check.collect(root, None)
            self.assertEqual(report["os"], "Test Linux 1.0")
            self.assertEqual(report["cpu"], "Test CPU 9000")
            self.assertEqual(report["kernel"], "6.99.0-test")
            self.assertEqual(len(report["amdGpus"]), 1)
            gpu = report["amdGpus"][0]
            self.assertEqual(gpu["gcVersion"], "12.0.1")
            self.assertEqual(gpu["resizableBar"], "full")
            self.assertEqual(gpu["pciSlot"], "0000:03:00.0")
            self.assertEqual(report["udmabuf"]["sizeLimitMb"], 64)
            self.assertEqual(report["ttmPagesLimit"], 3407872)
            texts = [text for _, text in host_check.verdicts(report)]
            self.assertTrue(any("gfx12 (RDNA4)" in t for t in texts))

    def test_limited_bar_is_a_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_host(root, 0)
            (root / "sys/class/drm/card1/device/mem_info_vis_vram_total").write_text(str(256 * 1024 ** 2))
            verdicts = host_check.verdicts(host_check.collect(root, None))
            self.assertIn(("FAIL", "card1: Resizable BAR limited"), verdicts)

    def test_non_amd_cards_are_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_host(root, 16, vendor="0x10de")
            report = host_check.collect(root, None)
            self.assertEqual(report["amdGpus"], [])
            self.assertIn(("FAIL", "no AMD GPU found under /sys/class/drm"), host_check.verdicts(report))

    def test_cli_writes_json_and_fails_without_render_node(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "root"
            fake_host(root, 16)
            out = Path(tmp) / "report.json"
            argv = ["host_check.py", "--root", str(root), "--json", str(out)]
            with patch("sys.argv", argv), contextlib.redirect_stdout(io.StringIO()) as printed:
                code = host_check.main()
            self.assertEqual(code, 1)
            self.assertIn("render node access: none", printed.getvalue())
            data = json.loads(out.read_text())
            self.assertEqual(data["amdGpus"][0]["card"], "card1")
            self.assertTrue(any(v["status"] == "FAIL" for v in data["verdicts"]))


if __name__ == "__main__":
    unittest.main()
