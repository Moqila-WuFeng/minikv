# SPDX-License-Identifier: Apache-2.0

"""Run a tiny real sync/async experiment in disposable databases."""
import json
import os
import pathlib
import signal
import subprocess
import sys
import tempfile


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def execute(command, timeout, temporary_root):
    environment = dict(os.environ, TMPDIR=str(temporary_root))
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, start_new_session=True, env=environment) as process:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except BaseException:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate()
            raise
        return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def main():
    runner, build_dir = map(pathlib.Path, sys.argv[1:3])
    check(runner.is_file(), "isolated benchmark runner is not implemented yet")
    with tempfile.TemporaryDirectory(prefix="minikv-runner-test-") as directory:
        report = pathlib.Path(directory) / "report.json"
        command = [sys.executable, str(runner), "--build-dir", str(build_dir),
                   "--output", str(report), "--requests", "13", "--keys", "3",
                   "--warmup", "2", "--concurrency", "2", "--operations", "mixed",
                   "--repeats", "2", "--case-timeout", "15"]
        result = execute(command, 60, directory)
        check(result.returncode == 0, result.stderr)
        data = json.loads(report.read_text())
        check(data["schema_version"] == 1 and len(data["runs"]) == 4, "two policies per repeat")
        check([run["sync_writes"] for run in data["runs"]] == [True, False, False, True],
              "policy order alternates between repeats")
        check(all(run["benchmark"]["succeeded"] == 13 for run in data["runs"]), "all calls succeed")
        check(all(run["server_exit_code"] == 0 for run in data["runs"]), "graceful process cleanup")
        check(data["environment"]["build_type"] in ("Debug", "Release", "RelWithDebInfo"),
              "record actual CMake build type")
        check(len(data["summary"]) == 2, "summary grouped by policy and workload")
        check(all(item["runs"] == 2 for item in data["summary"]), "repeats retained")
        original = report.read_bytes()
        result = execute(command, 10, directory)
        check(result.returncode != 0 and report.read_bytes() == original, "never overwrite reports")
        invalid = [*command[:-2], "--case-timeout", "0", "--output", str(report.with_name("invalid.json"))]
        result = execute(invalid, 10, directory)
        check(result.returncode == 2 and "out of range" in result.stderr, "reject nonpositive deadlines")
        timeout_report = pathlib.Path(directory) / "timed-out.json"
        result = execute([sys.executable, str(runner), "--build-dir", str(build_dir),
                                 "--output", str(timeout_report), "--keys", "1000000",
                                 "--operations", "put", "--concurrency", "1", "--repeats", "1",
                          "--case-timeout", "0.001"], 20, directory)
        check(result.returncode == 1 and not timeout_report.exists(),
              "deadline aborts the experiment without a misleading completed report")
        print("PASS: isolated runner, alternating policies, reports and non-overwrite")


if __name__ == "__main__":
    main()
