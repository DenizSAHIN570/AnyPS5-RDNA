import argparse
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FRAME_LINE = re.compile(r"^\[FrameTiming\] (.*)$")
FIELD = re.compile(r"(\w+)=([^\s(]+)")
FRAME_FIELDS = ("flip_interval_ms", "first_submit_to_flip_ms", "gpu_busy_ms", "gpu_gap_ms", "worker_span_ms", "worker_unattributed_ms", "packet_count", "packet_dwords", "slow_packet_ms")


def run_command(command, **kwargs):
    print("+ " + " ".join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def git(*arguments):
    result = subprocess.run(["git", "-C", str(ROOT), *arguments], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def build(args):
    configure = ["cmake", "-S", ROOT, "-B", args.build_dir, "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={args.build_type}",
                 "-DAPS5_ENABLE_TIMING_LOG=ON", "-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++"]
    run_command(configure + list(args.cmake_arg or []))
    run_command(["cmake", "--build", args.build_dir, "--parallel"])
    run_command(["cmake", "--build", args.build_dir, "--target", "libs", "--parallel"])
    return 0


def link_resources(app0, target):
    target.mkdir(parents=True, exist_ok=True)
    linked = 0
    for entry in sorted(app0.iterdir()):
        destination = target / entry.name
        if destination.exists() or destination.is_symlink():
            continue
        destination.symlink_to(entry.resolve(), target_is_directory=entry.is_dir())
        linked += 1
    return linked


def prepare(args):
    out = args.out
    if out.exists() and any(out.iterdir()) and not args.force:
        sys.exit(f"{out} is not empty; pass --force to reuse it")
    out.mkdir(parents=True, exist_ok=True)
    libs = args.libs
    prx = sorted(libs.glob("*.prx"))
    if not prx:
        sys.exit(f"no .prx files in {libs}: build the libs target first")
    relink = [args.relinker, "--registry", *(args.relinker_arg or []), args.input, out / "app.elf"]
    log = out / "relink.log"
    with log.open("w") as stream:
        print("+ " + " ".join(str(part) for part in relink), flush=True)
        result = subprocess.run([str(part) for part in relink], stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        sys.exit(f"relinker failed with exit code {result.returncode}; see {log}")
    (out / "libs").mkdir(exist_ok=True)
    for library in prx:
        shutil.copy2(library, out / "libs" / library.name)
    linked = link_resources(args.app0, out / "app0") if args.app0 else 0
    (out / "app.elf").chmod(0o755)
    manifest = {
        "input": str(args.input.resolve()),
        "relinker": str(Path(args.relinker).resolve()),
        "relinkerArgs": list(args.relinker_arg or []),
        "libs": str(libs.resolve()),
        "libraries": len(prx),
        "app0": str(args.app0.resolve()) if args.app0 else None,
        "linkedResources": linked,
        "commit": git("rev-parse", "HEAD"),
        "dirty": bool(git("status", "--porcelain")),
        "preparedAt": datetime.now(timezone.utc).isoformat(),
    }
    (out / "prepare.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"prepared {out}: {len(prx)} libraries, {linked} app0 entries linked")
    return 0


def parse_env(pairs):
    env = {}
    for pair in pairs or []:
        key, separator, value = pair.partition("=")
        if not separator or not key:
            sys.exit(f"--env needs KEY=VALUE, got {pair!r}")
        env[key] = value
    return env


def stop(process, grace):
    if process.poll() is not None:
        return "exited"
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=grace)
        return "interrupted"
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        return "killed"


def run(args):
    out = args.out
    executable = out / "app.elf"
    if not executable.exists():
        sys.exit(f"{executable} does not exist: run prepare first")
    label = args.label or datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = out / "runs" / label
    if run_dir.exists():
        sys.exit(f"{run_dir} already exists")
    run_dir.mkdir(parents=True)
    env = dict(os.environ)
    env["APS5_PROFILE_DRAW"] = "1"
    if args.dump_shaders:
        env["APS5_DUMP_REGISTERED_SHADERS"] = str((run_dir / "shaders").resolve())
    if args.memory_census:
        env["APS5_MEMORY_CENSUS"] = str((run_dir / "memory-census.csv").resolve())
    extra = parse_env(args.env)
    env.update(extra)
    timing = out / "frame-timing.log"
    if timing.exists():
        timing.unlink()
    started = time.monotonic()
    started_at = datetime.now(timezone.utc).isoformat()
    with (run_dir / "stdout.log").open("wb") as stdout, (run_dir / "stderr.log").open("wb") as stderr:
        process = subprocess.Popen([str(executable.resolve())], cwd=out, env=env, stdout=stdout, stderr=stderr)
        try:
            process.wait(timeout=args.seconds)
            ending = "exited"
        except subprocess.TimeoutExpired:
            ending = stop(process, args.grace)
        except KeyboardInterrupt:
            ending = stop(process, args.grace)
    elapsed = time.monotonic() - started
    if timing.exists():
        shutil.move(str(timing), run_dir / "frame-timing.log")
    metadata = {
        "label": label,
        "startedAt": started_at,
        "seconds": round(elapsed, 3),
        "requestedSeconds": args.seconds,
        "ending": ending,
        "exitCode": process.returncode,
        "environment": {key: env[key] for key in sorted(env) if key.startswith(("APS5_", "ANYPS5_", "RADV_", "MESA_", "AMD_")) or key in extra},
        "commit": git("rev-parse", "HEAD"),
        "dirty": bool(git("status", "--porcelain")),
    }
    if args.host_check:
        metadata["host"] = json.loads(args.host_check.read_text())
    prepared = out / "prepare.json"
    if prepared.exists():
        metadata["prepare"] = json.loads(prepared.read_text())
    (run_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    summary = summarize_run(run_dir)
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print_summary(run_dir, summary)
    return 0


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return None
    index = min(len(ordered) - 1, max(0, round(fraction * (len(ordered) - 1))))
    return ordered[index]


def frame_records(path):
    records = []
    if not path.exists():
        return records
    for line in path.read_text(errors="replace").splitlines():
        match = FRAME_LINE.match(line)
        if not match:
            continue
        fields = {}
        for key, value in FIELD.findall(match.group(1)):
            try:
                fields[key] = float(value)
            except ValueError:
                continue
        records.append(fields)
    return records


def describe(values):
    if not values:
        return None
    return {"count": len(values), "mean": statistics.fmean(values), "p50": percentile(values, 0.5), "p95": percentile(values, 0.95), "p99": percentile(values, 0.99), "max": max(values)}


def summarize_run(run_dir, skip_seconds=0.0):
    records = frame_records(run_dir / "frame-timing.log")
    if skip_seconds > 0 and records and "flip_complete_at_ms" in records[0]:
        first = records[0]["flip_complete_at_ms"]
        records = [r for r in records if r.get("flip_complete_at_ms", first) - first >= skip_seconds * 1000]
    summary = {"frames": len(records), "skippedSeconds": skip_seconds}
    for field in FRAME_FIELDS:
        values = [r[field] for r in records if field in r]
        summary[field] = describe(values)
    intervals = [r["flip_interval_ms"] for r in records if r.get("flip_interval_ms", 0) > 0]
    if intervals:
        mean = statistics.fmean(intervals)
        slowest = sorted(intervals, reverse=True)[:max(1, len(intervals) // 100)]
        summary["fps"] = {"average": 1000.0 / mean, "onePercentLow": 1000.0 / statistics.fmean(slowest)}
    stderr = run_dir / "stderr.log"
    if stderr.exists():
        lines = stderr.read_text(errors="replace").splitlines()
        summary["profileLines"] = {tag: sum(1 for l in lines if l.startswith(f"[{tag}]")) for tag in ("guestmem", "recorder", "lock", "gpu")}
        summary["lastProfile"] = {tag: next((l for l in reversed(lines) if l.startswith(f"[{tag}]")), None) for tag in ("guestmem", "recorder", "lock")}
    shaders = run_dir / "shaders"
    if shaders.is_dir():
        summary["dumpedShaders"] = sum(1 for _ in shaders.glob("*.ash"))
    census = run_dir / "memory-census.csv"
    if census.exists():
        summary["memoryCensusRows"] = max(0, sum(1 for _ in census.open()) - 1)
    return summary


def print_summary(run_dir, summary):
    print(f"run {run_dir}: {summary['frames']} frames")
    fps = summary.get("fps")
    if fps:
        print(f"  {fps['average']:.1f} fps average, {fps['onePercentLow']:.1f} fps 1% low")
    for field in ("flip_interval_ms", "gpu_busy_ms", "worker_span_ms", "packet_count"):
        stats = summary.get(field)
        if stats:
            print(f"  {field}: mean {stats['mean']:.3f}, p50 {stats['p50']:.3f}, p99 {stats['p99']:.3f}, max {stats['max']:.3f}")
    if "dumpedShaders" in summary:
        print(f"  dumped shaders: {summary['dumpedShaders']}")
    if "memoryCensusRows" in summary:
        print(f"  memory census rows: {summary['memoryCensusRows']}")


def summarize(args):
    summary = summarize_run(args.run_dir, args.skip_seconds)
    (args.run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print_summary(args.run_dir, summary)
    return 0


def main():
    parser = argparse.ArgumentParser(description="Build, prepare and run a relinked title for M0 baselines (issues #1 and #2).")
    commands = parser.add_subparsers(dest="command", required=True)

    p = commands.add_parser("build", help="configure and build upstream with frame timing, then the libs target")
    p.add_argument("--build-dir", type=Path, default=ROOT / "build")
    p.add_argument("--build-type", default="Release")
    p.add_argument("--cmake-arg", action="append", help="extra -D option for the configure step")
    p.set_defaults(handler=build)

    p = commands.add_parser("prepare", help="relink a title and assemble the runtime layout")
    p.add_argument("--relinker", type=Path, required=True)
    p.add_argument("--input", type=Path, required=True, help="the title's ELF, with sce_module/ beside it")
    p.add_argument("--libs", type=Path, default=ROOT / "build" / "core" / "libs" / "libs")
    p.add_argument("--app0", type=Path, help="the title's app0 resources, linked into OUT/app0")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--relinker-arg", action="append", help="extra relinker option")
    p.add_argument("--force", action="store_true")
    p.set_defaults(handler=prepare)

    p = commands.add_parser("run", help="run a prepared title with profiling and collect logs")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=120.0)
    p.add_argument("--grace", type=float, default=10.0, help="seconds between SIGINT and SIGKILL")
    p.add_argument("--label")
    p.add_argument("--env", action="append", help="extra KEY=VALUE for the title")
    p.add_argument("--dump-shaders", action="store_true", help="set APS5_DUMP_REGISTERED_SHADERS")
    p.add_argument("--memory-census", action="store_true", help="set APS5_MEMORY_CENSUS")
    p.add_argument("--host-check", type=Path, help="host_check.py --json output to embed")
    p.set_defaults(handler=run)

    p = commands.add_parser("summarize", help="summarize a run directory again")
    p.add_argument("run_dir", type=Path)
    p.add_argument("--skip-seconds", type=float, default=0.0, help="ignore frames in the first N seconds")
    p.set_defaults(handler=summarize)

    args = parser.parse_args()
    if args.command in ("prepare", "run") and sys.platform != "linux":
        sys.exit("baseline.py prepares and runs Linux output only")
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
