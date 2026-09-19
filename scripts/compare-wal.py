#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Compare WAL synchronization policies using isolated local server processes."""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import socket
import statistics
import subprocess
import sys
import tempfile
import time


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    home = pathlib.Path(os.environ.get("MINIKV_HOME", pathlib.Path.home() / "projects/minikv"))
    parser.add_argument("--build-dir", type=pathlib.Path, default=home / "build-service")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--requests", type=int, default=10000)
    parser.add_argument("--keys", type=int, default=1000)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--value-size", type=int, default=128)
    parser.add_argument("--concurrency", nargs="+", type=int, default=[1, 4, 16])
    parser.add_argument("--operations", nargs="+", choices=["get", "put", "mixed"],
                        default=["put", "get", "mixed"])
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--read-percent", type=int, default=50)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout-ms", type=int, default=2000)
    parser.add_argument("--case-timeout", type=float, default=120)
    parser.add_argument("--server-threads", type=int, default=4)
    parser.add_argument("--server-concurrency", type=int, default=64)
    args = parser.parse_args()
    valid = (1 <= args.requests <= 1000000 and 1 <= args.keys <= 1000000
             and 0 <= args.warmup <= 1000000 and 0 <= args.value_size <= 1048576
             and all(1 <= c <= 256 for c in args.concurrency)
             and 1 <= args.repeats <= 100 and 0 <= args.read_percent <= 100
             and 0 <= args.seed <= 0xffffffff and 1 <= args.timeout_ms <= 2147483647
             and 0 < args.case_timeout <= 86400 and 0 < args.server_threads <= 256
             and 0 < args.server_concurrency <= 1000000)
    if not valid:
        parser.error("benchmark arguments are out of range")
    if args.output.exists():
        parser.error("output already exists; choose a new report path")
    args.build_dir = args.build_dir.resolve()
    for name in ("minikv_server", "minikv_bench"):
        if not (args.build_dir / name).is_file():
            parser.error(f"missing executable: {name}")
    return args


def command_text(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        return result.stdout.strip() if result.returncode == 0 else "unavailable"
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"


def environment(build):
    cache = {}
    cache_file = build / "CMakeCache.txt"
    if cache_file.exists():
        for line in cache_file.read_text().splitlines():
            if line and not line.startswith(("#", "//")) and "=" in line:
                key, value = line.split("=", 1)
                cache[key.split(":", 1)[0]] = value
    source = cache.get("CMAKE_HOME_DIRECTORY")
    revision = command_text(["git", "-C", source, "rev-parse", "HEAD"]) if source else "unavailable"
    status = command_text(["git", "-C", source, "status", "--porcelain"]) if source else "unavailable"
    cpu = "unavailable"
    cpuinfo = pathlib.Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    hashes = {}
    for name in ("minikv_server", "minikv_bench"):
        with (build / name).open("rb") as binary:
            hashes[name] = hashlib.file_digest(binary, "sha256").hexdigest()
    return {
        "system": platform.system(), "kernel": platform.release(),
        "architecture": platform.machine(), "logical_cpus": os.cpu_count(), "cpu_model": cpu,
        "python": platform.python_version(), "build_type": cache.get("CMAKE_BUILD_TYPE", "unknown"),
        "compiler": command_text([cache.get("CMAKE_CXX_COMPILER", "c++"), "--version"]).splitlines()[0],
        "source_checkout_revision": revision,
        "source_checkout_dirty": None if status == "unavailable" else status != "",
        "temporary_filesystem": command_text(["stat", "-f", "-c", "%T", tempfile.gettempdir()]),
        "brpc_version": "1.18.0",
        "package_versions": command_text(["dpkg-query", "-W", "librocksdb-dev", "libprotobuf-dev",
                                           "libgflags-dev"]),
        "binary_sha256": hashes,
    }


def run_case(args, operation, concurrency, sync, repeat):
    with tempfile.TemporaryDirectory(prefix="minikv-wal-bench-") as directory:
        root = pathlib.Path(directory)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        endpoint = f"127.0.0.1:{port}"
        with (root / "server.log").open("wb") as log:
            server = subprocess.Popen(
                [str(args.build_dir / "minikv_server"), f"--listen={endpoint}",
                 f"--db_path={root / 'db'}", f"--sync_writes={str(sync).lower()}",
                 f"--worker_threads={args.server_threads}",
                 f"--max_concurrency={args.server_concurrency}"], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while True:
                    if server.poll() is not None:
                        raise RuntimeError("benchmark server exited during startup")
                    # A free-port probe alone can connect to an unrelated process.
                    lines = (root / "server.log").read_text(errors="replace").splitlines()
                    announced = any(line.startswith(f"MiniKV listening on {endpoint}, db=") for line in lines)
                    try:
                        if not announced:
                            raise OSError("waiting for our child's startup announcement")
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise RuntimeError("benchmark server startup timed out")
                        time.sleep(0.05)
                command = [str(args.build_dir / "minikv_bench"), f"--server={endpoint}",
                           f"--key_prefix={root.name}",
                           f"--operation={operation}", f"--concurrency={concurrency}",
                           f"--requests={args.requests}", f"--keys={args.keys}",
                           f"--warmup={args.warmup}", f"--value_size={args.value_size}",
                           f"--read_percent={args.read_percent}", f"--seed={args.seed}",
                           f"--timeout_ms={args.timeout_ms}"]
                result = subprocess.run(command, capture_output=True, text=True, timeout=args.case_timeout)
                if result.returncode != 0:
                    raise RuntimeError(f"benchmark failed ({result.returncode}): {result.stdout}\n{result.stderr}")
                report = json.loads(result.stdout)
                if report["succeeded"] != args.requests:
                    raise RuntimeError("benchmark did not complete all requests successfully")
                server.terminate()
                code = server.wait(timeout=15)
                if code != 0:
                    raise RuntimeError(f"benchmark server shutdown failed: {code}")
                return {"repeat": repeat, "sync_writes": sync, "benchmark": report,
                        "server_exit_code": code, "server_threads": args.server_threads,
                        "server_max_concurrency": args.server_concurrency}
            except Exception:
                log.flush()
                print((root / "server.log").read_text(errors="replace")[-4000:], file=sys.stderr)
                raise
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait(timeout=5)


def summarize(runs):
    groups = {}
    for run in runs:
        report = run["benchmark"]
        key = (report["operation"], report["concurrency"], run["sync_writes"])
        groups.setdefault(key, []).append(report)
    summaries = []
    for (operation, concurrency, sync), reports in groups.items():
        rates = [report["success_ops_per_second"] for report in reports]
        summaries.append({"operation": operation, "concurrency": concurrency, "sync_writes": sync,
                          "runs": len(reports), "success_ops_per_second_median": statistics.median(rates),
                          "success_ops_per_second_min": min(rates), "success_ops_per_second_max": max(rates),
                          "median_of_run_p99_us": statistics.median(
                              report["attempt_latency_us"]["p99"] for report in reports)})
    return summaries


def main():
    args = arguments()
    metadata = environment(args.build_dir)
    runs = []
    for repeat in range(1, args.repeats + 1):
        for operation in args.operations:
            for concurrency in args.concurrency:
                for sync in ((True, False) if repeat % 2 else (False, True)):
                    print(f"repeat={repeat} operation={operation} concurrency={concurrency} sync={sync}",
                          file=sys.stderr)
                    runs.append(run_case(args, operation, concurrency, sync, repeat))
    report = {"schema_version": 1, "created_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "environment": metadata, "runs": runs, "summary": summarize(runs),
              "methodology": "closed-loop, loopback, fresh DB per case, prefill and warmup excluded"}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2, allow_nan=False)
        output.write("\n")
    print(f"Report: {args.output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Comparison failed: {error}", file=sys.stderr)
        sys.exit(1)
