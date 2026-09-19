# SPDX-License-Identifier: Apache-2.0

"""Black-box checks for benchmark accounting, validation and failures."""
import json
import math
import pathlib
import socket
import subprocess
import sys
import tempfile
import time


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    server_bin, bench_bin, client_bin = map(pathlib.Path, sys.argv[1:4])
    check(bench_bin.is_file(), "minikv_bench is not implemented yet")
    with tempfile.TemporaryDirectory(prefix="minikv-benchmark-test-") as directory:
        root = pathlib.Path(directory)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        endpoint = f"127.0.0.1:{port}"
        with (root / "server.log").open("wb") as log:
            server = subprocess.Popen([str(server_bin), f"--listen={endpoint}",
                                       f"--db_path={root / 'db'}"], stdout=log, stderr=log)
            base = [str(bench_bin), f"--server={endpoint}", "--requests=23",
                    "--concurrency=4", "--keys=7", "--warmup=5", "--value_size=256",
                    "--key_prefix=test", "--timeout_ms=300"]

            def run(*args, expected=0):
                result = subprocess.run([*base, *args], capture_output=True, text=True, timeout=15)
                check(result.returncode == expected, f"{args}: {result.returncode}: {result.stderr}")
                return result

            def report(*args, expected=0):
                data = json.loads(run(*args, expected=expected).stdout)
                check(data["attempted"] == 23, "measured requests exclude prefill and warmup")
                check(data["get_attempted"] + data["put_attempted"] == 23, "operation accounting")
                check(data["succeeded"] + data["rpc_errors"] + data["application_errors"]
                      + data["validation_errors"] == 23, "outcome accounting")
                check(data["elapsed_seconds"] > 0, "positive duration")
                check(math.isclose(data["success_ops_per_second"],
                                   data["succeeded"] / data["elapsed_seconds"], rel_tol=1e-9), "QPS")
                latency = data["attempt_latency_us"]
                check(0 <= latency["min"] <= latency["p50"] <= latency["p95"]
                      <= latency["p99"] <= latency["max"], "ordered percentiles")
                check(latency["samples"] == 23, "one sample per attempt")
                return data

            try:
                deadline = time.monotonic() + 15
                while True:
                    check(server.poll() is None, "server startup failed")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        check(time.monotonic() < deadline, "server startup timeout")
                        time.sleep(0.05)
                for operation in ("put", "get", "mixed"):
                    data = report(f"--operation={operation}")
                    check(data["succeeded"] == 23, "all benchmark requests succeed")
                    check(data["prefill_completed"] == 7 and data["warmup_completed"] == 5,
                          "setup counted separately")
                    if operation != "mixed":
                        check(data[f"{operation}_attempted"] == 23, "single operation workload")
                for percent in (0, 100):
                    data = report("--operation=mixed", f"--read_percent={percent}")
                    check(data["get_attempted"] == (23 if percent else 0), "mix endpoints")
                data = report("--operation=get", "--prefill=false", "--warmup=0",
                              "--key_prefix=absent", expected=1)
                check(data["application_errors"] == 23, "NOT_FOUND is not throughput success")
                failed_warmup = run("--operation=get", "--prefill=false", "--key_prefix=absent",
                                    expected=1)
                check(not failed_warmup.stdout and "Warmup failed" in failed_warmup.stderr,
                      "failed warmup must not produce a measured report")

                # A successful RPC with wrong data is still a benchmark failure.
                subprocess.run([str(client_bin), f"--server={endpoint}", "put", "wrong:0", "bad"],
                               check=True, capture_output=True, timeout=5)
                data = report("--operation=get", "--prefill=false", "--warmup=0",
                              "--keys=1", "--key_prefix=wrong", expected=1)
                check(data["validation_errors"] == 23, "read payload verified")
                for argument in ("--requests=0", "--requests=1000001", "--concurrency=0",
                                 "--concurrency=257", "--keys=0", "--keys=1000001", "--warmup=-1",
                                 "--value_size=-1", "--value_size=1048577", "--operation=bad",
                                 "--read_percent=101", "--timeout_ms=0", "--key_prefix=",
                                 "--key_prefix=" + "x" * 901):
                    run(argument, expected=2)
                data = json.loads(run("--requests=1", "--concurrency=8", "--value_size=0").stdout)
                check(data["attempted"] == data["succeeded"] == 1, "small request count and empty value")
                data = json.loads(run("--requests=1", "--keys=1", "--value_size=1048576").stdout)
                check(data["succeeded"] == 1, "maximum binary value size")
                with open("/dev/full", "wb") as full:
                    result = subprocess.run(base, stdout=full, stderr=subprocess.PIPE, timeout=15)
                check(result.returncode == 1, "stdout failure is not success")
                server.terminate()
                check(server.wait(timeout=10) == 0, "server graceful shutdown")
                data = report("--prefill=false", "--warmup=0", expected=1)
                check(data["rpc_errors"] == 23, "transport failures counted")
                run(expected=1)
                print("PASS: benchmark workloads, counts, errors, payload validation and boundaries")
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait(timeout=5)


if __name__ == "__main__":
    main()
