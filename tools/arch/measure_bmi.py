#!/usr/bin/env python3
"""Producer BMI / PSS measurement (RFC 0001 Phase E).

Reproducibly compile ONE module interface translation unit and record:

  * producer peak PSS (kB) sampled from /proc/<pid>/smaps_rollup while the
    compiler process runs (the RFC metric - PSS, never RSS);
  * emitted reduced-BMI bytes (the `-fmodule-output=*.pcm` path from the
    unit's clang module map) and object bytes;
  * producer wall time and (secondary, cross-check only) ru_maxrss.

The exact compile argv comes from <build>/compile_commands.json and the
exact @<obj>.modmap, so the measurement matches what ninja runs - same
compiler, flags, cwd and module map. No ad-hoc /tmp scripts.

Linux-only: PSS needs /proc. On macOS the tool exits 3; macos-14 evidence
is CI wall/step timing at default Ninja parallelism (RFC §4.5). It is NOT
run by the Architecture check workflow (that stays a fast static lint).

Usage:
  tools/arch/measure_bmi.py src/query/query_engine.cppm
  tools/arch/measure_bmi.py --mode ninja --force --runs 3 query_engine
  tools/arch/measure_bmi.py --json --out /tmp/before.json src/services/mcp/client.cppm

"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_PLATFORM = 3

POLL_INTERVAL_MS_DEFAULT = 20


def die_usage(msg: str) -> "NoReturn":  # type: ignore[name-defined]
    print(f"measure_bmi: {msg}", file=sys.stderr)
    sys.exit(EXIT_USAGE)


def git_info() -> dict:
    def run(args: list[str]) -> str:
        r = subprocess.run(["git", "-C", str(ROOT)] + args,
                           capture_output=True, text=True)
        return r.stdout.strip()
    return {"root": str(ROOT),
            "git_head": run(["rev-parse", "HEAD"]),
            "git_dirty": bool(run(["status", "--porcelain"]))}


def read_pss_kb(pid: int) -> int | None:
    try:
        text = Path(f"/proc/{pid}/smaps_rollup").read_text()
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return None
    for line in text.splitlines():
        if line.startswith("Pss:"):
            return int(line.split()[1])
    return None


def find_clang_pids_for(source_abs: str) -> set[int]:
    """clang++ PIDs whose NUL cmdline names source_abs (excludes scan-deps
    and clangd)."""
    pids: set[int] = set()
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        try:
            comm = (entry / "comm").read_text().strip()
            cmd = (entry / "cmdline").read_bytes().decode("utf-8", "replace")
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
        # The producer is clang++ (also reported as "clang-<ver>"); exclude
        # clangd (editor indexer) and clang-scan-deps explicitly.
        if not (comm == "clang++" or comm.startswith("clang-")):
            continue
        argv = cmd.split("\0")
        if source_abs in argv and not any("clang-scan-deps" in a for a in argv):
            pids.add(pid)
    return pids


def resolve_entry(source: str, build_dir: Path) -> tuple[dict, Path]:
    cc = build_dir / "compile_commands.json"
    if not cc.exists():
        die_usage(f"{cc} missing - configure with --preset local-linux first "
                  f"(or pass --build-dir).")
    db = json.loads(cc.read_text())
    wanted = os.path.normpath(source)
    abs_wanted = wanted if os.path.isabs(wanted) else os.path.normpath(ROOT / wanted)
    matches = []
    for entry in db:
        f = os.path.normpath(entry.get("file", ""))
        if f == abs_wanted or f == wanted or f.endswith(os.sep + wanted):
            matches.append(entry)
            continue
        # Unique basename / basename-prefix shorthand (e.g. "client.cppm"
        # or "query_engine"); still fail-closed if more than one matches.
        if Path(f).name == wanted or Path(f).stem == source:
            matches.append(entry)
    if not matches:
        die_usage(f"no compile_commands entry for {source!r} in {cc}.")
    files = sorted({m["file"] for m in matches})
    if len(files) > 1:
        die_usage(f"{source!r} matches {len(files)} entries; use a path "
                  f"suffix that is unique:\n  " + "\n  ".join(files))
    entry = next(m for m in matches if m["file"] == files[0])
    return entry, Path(entry["directory"])


def parse_compile(entry: dict, cwd: Path) -> tuple[list[str], str, str | None, str | None]:
    """Return (argv, obj_rel, modmap_path_or_None, bmi_rel_or_None)."""
    argv = shlex.split(entry["command"])
    obj_rel = entry.get("output", "")
    modmap_path = None
    bmi_rel = None
    for tok in argv:
        if tok.startswith("@"):
            modmap_path = tok[1:]
            break
    if modmap_path:
        mm = (cwd / modmap_path).resolve()
        if not mm.exists():
            die_usage(f"module map {modmap_path} missing - run ninja once "
                      f"or use --mode ninja.")
        try:
            modmap_text = mm.read_text()
        except OSError as e:
            die_usage(f"cannot read module map {mm}: {e}")
        for line in modmap_text.splitlines():
            line = line.strip()
            if line.startswith("-fmodule-output="):
                bmi_rel = line.split("=", 1)[1]
                break
    return argv, obj_rel, modmap_path, bmi_rel


def compiler_info(argv: list[str]) -> dict:
    cxx = argv[0] if argv else "clang++"
    version = ""
    if shutil.which(cxx) or Path(cxx).exists():
        r = subprocess.run([cxx, "--version"], capture_output=True, text=True)
        version = r.stdout.splitlines()[0] if r.stdout else ""
    return {"path": cxx, "version": version}


def stat_bytes(cwd: Path, rel: str | None) -> int | None:
    if not rel:
        return None
    p = (cwd / rel) if not os.path.isabs(rel) else Path(rel)
    try:
        return p.stat().st_size
    except OSError:
        return None


def run_direct(entry: dict, cwd: Path, interval_s: float) -> dict:
    argv, obj_rel, _, _ = parse_compile(entry, cwd)
    # Safety: compile_commands in this tree carry no shell operators.
    if any(any(c in t for c in ";&|<>$\n`") for t in argv if not t.startswith("-")):
        die_usage("refusing to exec a compile token with shell metacharacters.")
    if not (Path(argv[0]).exists() or shutil.which(argv[0])):
        die_usage(f"compiler not found: {argv[0]}")
    t0 = time.perf_counter()
    # Popen runs the compiler directly (no shell wrapper) with cwd set, so
    # relative @modmap/-o resolve and proc.pid IS the producer. PIPE lets us
    # report the clang tail on failure; a reader thread keeps it drained.
    proc = subprocess.Popen(argv, cwd=str(cwd),
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    captured: dict[str, bytes] = {"err": b""}

    def _drain_err():
        captured["err"] = proc.stderr.read() if proc.stderr else b""
        if proc.stderr:
            proc.stderr.close()

    reader = threading.Thread(target=_drain_err, daemon=True)
    reader.start()
    peak = 0
    samples = 0
    while proc.poll() is None:
        kb = read_pss_kb(proc.pid)
        if kb is not None:
            samples += 1
            peak = max(peak, kb)
        time.sleep(interval_s)
    reader.join()
    wall = time.perf_counter() - t0
    rc = proc.returncode
    # ru_maxrss is unavailable via Popen; record None (PSS is the metric).
    return {"compile_rc": rc, "wall_s": round(wall, 4),
            "wall_scope": "producer", "pss_peak_kb": peak,
            "pss_samples": samples, "rss_max_kb_secondary": None,
            "ru_utime_s": None, "ru_stime_s": None,
            "error_tail": captured["err"].decode("utf-8", "replace")[-4096:]
                          if rc else ""}


def run_ninja(entry: dict, cwd: Path, build_dir: Path, obj_rel: str,
              source_abs: str, interval_s: float, force: bool) -> dict:
    # Remove the object (and optionally the BMI) so the producer recompiles.
    obj_path = cwd / obj_rel
    if obj_path.exists():
        obj_path.unlink()
    if force:
        _, _, _, bmi_rel = parse_compile(entry, cwd)
        bmi_bytes_path = cwd / bmi_rel if bmi_rel else None
        if bmi_bytes_path and bmi_bytes_path.exists():
            bmi_bytes_path.unlink()
    # No -j: never throttle default Ninja parallelism.
    cmd = ["ninja", "-C", str(build_dir), obj_rel]
    if shutil.which(cmd[0]) is None:
        die_usage("ninja not found on PATH.")
    t0 = time.perf_counter()
    # Drain ninja's combined stdout/stderr in a background thread so a large
    # prerequisite rebuild cannot fill the pipe buffer and deadlock while we
    # poll (Popen with an undrained PIPE blocks at ~64 KB).
    proc = subprocess.Popen(cmd, cwd=str(ROOT),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    captured: dict[str, bytes] = {"out": b""}

    def _drain():
        captured["out"] = proc.stdout.read() if proc.stdout else b""
        proc.stdout.close() if proc.stdout else None

    reader = threading.Thread(target=_drain, daemon=True)
    reader.start()
    peak = 0
    samples = 0
    while proc.poll() is None:
        pids = find_clang_pids_for(source_abs)
        total = 0
        for pid in pids:
            kb = read_pss_kb(pid)
            if kb is not None:
                total += kb
        if total > peak:
            peak = total
        samples += 1
        time.sleep(interval_s)
    reader.join()
    wall = time.perf_counter() - t0
    out = captured["out"].decode("utf-8", "replace")
    rc = proc.returncode
    return {"compile_rc": rc, "wall_s": round(wall, 4),
            "wall_scope": "ninja-target", "pss_peak_kb": peak,
            "pss_samples": samples, "rss_max_kb_secondary": None,
            "ru_utime_s": None, "ru_stime_s": None,
            "error_tail": (out[-4096:] if rc else "")}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="path / repo-relative path / unique basename "
                                   "or stem (or path suffix) of a "
                                   "compile_commands.json entry")
    ap.add_argument("--build-dir", default=None)
    ap.add_argument("--mode", choices=["direct", "ninja"], default="direct")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--interval-ms", type=int, default=POLL_INTERVAL_MS_DEFAULT)
    ap.add_argument("--force", action="store_true",
                    help="ninja mode: also remove the emitted .pcm")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    if sys.platform != "linux":
        print("PSS measurement is Linux-only: /proc/<pid>/smaps_rollup does "
              "not exist on macOS. macos-14 evidence is CI wall/step timing "
              "at default Ninja parallelism (RFC 0001 §4.5).", file=sys.stderr)
        return EXIT_PLATFORM
    if args.runs < 1:
        die_usage("--runs must be >= 1.")
    if args.interval_ms < 1:
        die_usage("--interval-ms must be >= 1 (a 0/negative value busy-spins).")

    if args.build_dir:
        build_dir = Path(args.build_dir)
    elif (ROOT / "build/debug").exists():
        build_dir = ROOT / "build/debug"
    elif (ROOT / "build/release").exists():
        build_dir = ROOT / "build/release"
    else:
        die_usage("no build/debug or build/release; configure with "
                  "--preset local-linux (or local-linux-release).")
    if not build_dir.is_absolute():
        build_dir = (ROOT / build_dir).resolve()

    entry, cwd = resolve_entry(args.source, build_dir)
    source_abs = os.path.normpath(entry["file"])
    argv, obj_rel, modmap_rel, bmi_rel = parse_compile(entry, cwd)
    module = Path(bmi_rel).stem if bmi_rel else Path(source_abs).stem

    if args.mode == "direct" and (not modmap_rel or not (cwd / modmap_rel).exists()):
        die_usage("build tree incomplete (no .modmap); run ninja once or use "
                  "--mode ninja.")

    interval_s = args.interval_ms / 1000.0
    runs = []
    for _ in range(max(1, args.runs)):
        if args.mode == "direct":
            runs.append(run_direct(entry, cwd, interval_s))
        else:
            runs.append(run_ninja(entry, cwd, build_dir, obj_rel, source_abs,
                                 interval_s, args.force))

    peaks = [r["pss_peak_kb"] for r in runs]
    rc = max(r["compile_rc"] for r in runs)
    # Only trust artifact sizes after a successful compile (direct mode does
    # not unlink outputs, so a failed run would otherwise report stale bytes).
    obj_bytes = stat_bytes(cwd, obj_rel) if rc == 0 else None
    bmi_bytes = stat_bytes(cwd, bmi_rel) if rc == 0 else None
    median = sorted(peaks)[len(peaks) // 2]
    zero_sample = runs[0]["pss_samples"] == 0

    record = {
        "schema_version": 1,
        "tool": "tools/arch/measure_bmi.py",
        "generated_utc": _dt.datetime.now(_dt.timezone.utc)
                            .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "platform": {"system": platform.system(), "release": platform.release(),
                     "machine": platform.machine(),
                     "python": platform.python_version()},
        "repo": git_info(),
        "target": {"source": os.path.relpath(source_abs, ROOT),
                   "module": module, "object_rel": obj_rel,
                   "bmi_rel": bmi_rel},
        "compiler": compiler_info(argv),
        "mode": args.mode,
        "interval_ms": args.interval_ms,
        "artifacts": {"object_bytes": obj_bytes, "bmi_bytes": bmi_bytes},
        "measurement": {
            "pss_peak_kb": max(peaks),
            "pss_peak_mb": round(max(peaks) / 1024.0, 1),
            "pss_note": "peak /proc smaps_rollup Pss (RFC metric); "
                        "polled lower bound, never RSS",
            "pss_median_kb_upper_middle": median,
            "pss_samples_first_run": runs[0]["pss_samples"],
            "wall_s_first_run": runs[0]["wall_s"],
            "wall_scope": runs[0]["wall_scope"],
        },
        "runs": runs if args.runs > 1 else [],
        "compile_rc": rc,
    }
    if zero_sample and not rc:
        print("warning: 0 PSS samples (TU faster than the poll interval); "
              "lower --interval-ms or use --mode ninja", file=sys.stderr)

    if args.json:
        text = json.dumps(record, indent=2)
        print(text)
    else:
        m = record["measurement"]
        print(f"# BMI measurement - {record['target']['module']} "
              f"({record['mode']}, {record['compiler']['version']})")
        print(f"peak PSS        : {m['pss_peak_mb']} MB "
              f"({m['pss_peak_kb']} kB, {m['pss_samples_first_run']} samples "
              f"@{args.interval_ms} ms; {m['pss_note']})")
        if bmi_bytes is not None:
            print(f"BMI .pcm bytes  : {bmi_bytes:,}  ({record['target']['bmi_rel']})")
        if obj_bytes is not None:
            print(f"object bytes    : {obj_bytes:,}  ({record['target']['object_rel']})")
        print(f"wall ({m['wall_scope']:>12}): {m['wall_s_first_run']} s")
        print(f"git             : {record['repo']['git_head'][:10]}"
              f"{' (dirty)' if record['repo']['git_dirty'] else ''}")
        if rc:
            print(f"COMPILE FAILED rc={rc}; clang stderr tail:", file=sys.stderr)
            print(runs[0].get("error_tail", "") or
                  "(no stderr captured; diagnostics may have streamed above)",
                  file=sys.stderr)
    if args.out:
        Path(args.out).write_text(json.dumps(record, indent=2) + "\n")
    return EXIT_OK if rc == 0 else rc


if __name__ == "__main__":
    sys.exit(main())
