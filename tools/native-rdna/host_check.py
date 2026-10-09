import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

AMD_VENDOR = "0x1002"


def read(root, path):
    try:
        return (root / path.lstrip("/")).read_text().strip()
    except (OSError, UnicodeDecodeError):
        return None


def number(text):
    try:
        return int(text, 0)
    except (TypeError, ValueError):
        return None


def accessible(root, path, mode):
    target = root / path.lstrip("/")
    return target.exists() and os.access(target, mode)


def os_release(root):
    text = read(root, "/etc/os-release") or ""
    for line in text.splitlines():
        if line.startswith("PRETTY_NAME="):
            return line.split("=", 1)[1].strip().strip('"')
    return None


def cpu_model(root):
    text = read(root, "/proc/cpuinfo") or ""
    for line in text.splitlines():
        if line.startswith("model name"):
            return line.split(":", 1)[1].strip()
    return None


def gc_version(device):
    base = device / "ip_discovery" / "die" / "0" / "GC" / "0"
    parts = []
    for name in ("major", "minor", "revision"):
        try:
            parts.append(int((base / name).read_text().strip()))
        except (OSError, ValueError):
            return None
    return ".".join(str(p) for p in parts)


def gpus(root):
    found = []
    drm = root / "sys" / "class" / "drm"
    if not drm.is_dir():
        return found
    for card in sorted(drm.iterdir()):
        if not card.name.startswith("card") or "-" in card.name:
            continue
        device = card / "device"
        vendor = read(root, str((device / "vendor").relative_to(root)))
        if vendor != AMD_VENDOR:
            continue
        rel = lambda name: str((device / name).relative_to(root))
        vram = number(read(root, rel("mem_info_vram_total")))
        visible = number(read(root, rel("mem_info_vis_vram_total")))
        gtt = number(read(root, rel("mem_info_gtt_total")))
        entry = {
            "card": card.name,
            "deviceId": read(root, rel("device")),
            "pciSlot": (read(root, rel("uevent")) or "").partition("PCI_SLOT_NAME=")[2].split("\n")[0] or None,
            "gcVersion": gc_version(device),
            "vramBytes": vram,
            "visibleVramBytes": visible,
            "gttBytes": gtt,
        }
        if vram and visible is not None:
            entry["resizableBar"] = "full" if visible * 10 >= vram * 9 else "limited"
        else:
            entry["resizableBar"] = "unknown"
        found.append(entry)
    return found


def module_parameter(root, module, parameter):
    return read(root, f"/sys/module/{module}/parameters/{parameter}")


def llvm_gfx1201():
    tool = shutil.which("llvm-mc")
    if tool is None:
        return {"llvmMc": None, "assemblesGfx1201": False}
    version = subprocess.run([tool, "--version"], capture_output=True, text=True).stdout
    line = next((l.strip() for l in version.splitlines() if "version" in l.lower()), None)
    with tempfile.TemporaryDirectory() as work:
        source = Path(work) / "probe.s"
        source.write_text("s_wait_kmcnt 0\ns_endpgm\n")
        result = subprocess.run([tool, "-triple=amdgcn-amd-amdhsa", "-mcpu=gfx1201", "-filetype=obj", str(source), "-o", str(Path(work) / "probe.o")], capture_output=True, text=True)
    return {"llvmMc": line, "assemblesGfx1201": result.returncode == 0}


def hsa_runtime():
    try:
        output = subprocess.run(["ldconfig", "-p"], capture_output=True, text=True).stdout
    except OSError:
        return False
    return "libhsa-runtime64" in output


def vram_bench_info(binary):
    if binary is None:
        return None
    with tempfile.TemporaryDirectory() as work:
        report = Path(work) / "info.json"
        result = subprocess.run([str(binary), "info", "--json", str(report)], capture_output=True, text=True)
        if result.returncode != 0:
            return {"error": (result.stderr or result.stdout).strip()}
        return json.loads(report.read_text())["info"]


def collect(root, vram_bench):
    report = {
        "kernel": platform.release() if root == Path("/") else read(root, "/proc/sys/kernel/osrelease"),
        "os": os_release(root),
        "cpu": cpu_model(root),
        "amdGpus": gpus(root),
        "renderNodes": sorted(p.name for p in (root / "dev" / "dri").glob("renderD*") if os.access(p, os.R_OK | os.W_OK)) if (root / "dev" / "dri").is_dir() else [],
        "udmabuf": {
            "device": accessible(root, "/dev/udmabuf", os.R_OK | os.W_OK),
            "sizeLimitMb": number(module_parameter(root, "udmabuf", "size_limit_mb")),
            "listLimit": number(module_parameter(root, "udmabuf", "list_limit")),
        },
        "ttmPagesLimit": number(module_parameter(root, "ttm", "pages_limit")),
        "amdgpuGttSizeMb": number(module_parameter(root, "amdgpu", "gttsize")),
        "kfd": accessible(root, "/dev/kfd", os.R_OK | os.W_OK),
    }
    if root == Path("/"):
        report["hsaRuntime"] = hsa_runtime()
        report["llvm"] = llvm_gfx1201()
        report["vulkan"] = vram_bench_info(vram_bench)
    return report


def verdicts(report):
    lines = []
    amd = report["amdGpus"]
    if not amd:
        lines.append(("FAIL", "no AMD GPU found under /sys/class/drm"))
    for gpu in amd:
        target = "gfx12 (RDNA4)" if (gpu["gcVersion"] or "").startswith("12.") else f"GC {gpu['gcVersion']}"
        lines.append(("info", f"{gpu['card']} {gpu['deviceId']} {target}, VRAM {fmt(gpu['vramBytes'])}, CPU-visible {fmt(gpu['visibleVramBytes'])}, GTT {fmt(gpu['gttBytes'])}"))
        lines.append(("ok" if gpu["resizableBar"] == "full" else "FAIL", f"{gpu['card']}: Resizable BAR {gpu['resizableBar']}"))
    lines.append(("ok" if report["renderNodes"] else "FAIL", "render node access: " + (", ".join(report["renderNodes"]) or "none")))
    udmabuf = report["udmabuf"]
    lines.append(("ok" if udmabuf["device"] else "warn", f"/dev/udmabuf {'usable' if udmabuf['device'] else 'not usable'} (size_limit_mb={udmabuf['sizeLimitMb']}, list_limit={udmabuf['listLimit']})"))
    lines.append(("info", f"ttm pages_limit={report['ttmPagesLimit']}, amdgpu gttsize={report['amdgpuGttSizeMb']}"))
    lines.append(("ok" if report["kfd"] else "warn", "/dev/kfd " + ("usable (hardware oracle)" if report["kfd"] else "not usable: the hardware oracle needs the render group")))
    if "hsaRuntime" in report:
        lines.append(("ok" if report["hsaRuntime"] else "warn", "HSA runtime " + ("found" if report["hsaRuntime"] else "missing (libhsa-runtime-dev / rocm-runtime-devel)")))
        llvm = report["llvm"]
        lines.append(("ok" if llvm["assemblesGfx1201"] else "warn", f"llvm-mc {llvm['llvmMc'] or 'not found'}: gfx1201 " + ("supported" if llvm["assemblesGfx1201"] else "not supported")))
        vulkan = report["vulkan"]
        if vulkan is None:
            lines.append(("warn", "vram_bench not given (--vram-bench): Vulkan checks skipped"))
        elif "error" in vulkan:
            lines.append(("FAIL", "vram_bench info failed: " + vulkan["error"]))
        else:
            lines.append(("info", f"Vulkan device {vulkan['device']} ({vulkan['driverInfo']})"))
            lines.append(("ok" if vulkan["fullResizableBar"] else "FAIL", "Vulkan sees a CPU-visible VRAM heap" if vulkan["fullResizableBar"] else "Vulkan sees no full CPU-visible VRAM heap"))
            lines.append(("ok" if vulkan["mapMemoryPlaced"] else "FAIL", "VK_EXT_map_memory_placed " + ("supported" if vulkan["mapMemoryPlaced"] else "missing")))
    return lines


def fmt(value):
    if value is None:
        return "?"
    return f"{value / (1024 ** 3):.1f} GiB"


def main():
    parser = argparse.ArgumentParser(description="Check a Linux host for the native RDNA4 experiments (issues #1 and #2).")
    parser.add_argument("--json", type=Path, help="write the full report here")
    parser.add_argument("--vram-bench", type=Path, help="vram_bench binary for the Vulkan checks")
    parser.add_argument("--root", type=Path, default=Path("/"), help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.root == Path("/") and sys.platform != "linux":
        sys.exit("host_check.py inspects a Linux host")
    report = collect(args.root, args.vram_bench)
    lines = verdicts(report)
    for status, text in lines:
        print(f"[{status:>4}] {text}")
    report["verdicts"] = [{"status": s, "text": t} for s, t in lines]
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")
    return 1 if any(s == "FAIL" for s, _ in lines) else 0


if __name__ == "__main__":
    sys.exit(main())
