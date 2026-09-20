# SPDX-License-Identifier: Apache-2.0

"""Exercise real RPC processes and an isolated RocksDB database."""
import concurrent.futures
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
    server_bin, client_bin = map(pathlib.Path, sys.argv[1:3])
    check(server_bin.is_file() and client_bin.is_file(), "RPC executables not implemented yet")
    with tempfile.TemporaryDirectory(prefix="minikv-rpc-") as directory:
        root = pathlib.Path(directory)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        endpoint = f"127.0.0.1:{port}"
        server = None
        log = (root / "server.log").open("wb")

        def call(*arguments, expected=0, extra=()):
            result = subprocess.run(
                [str(client_bin), f"--server={endpoint}", "--timeout_ms=3000", *extra, *arguments],
                capture_output=True, timeout=10,
            )
            check(result.returncode == expected,
                  f"{arguments}: exit={result.returncode}, stderr={result.stderr!r}")
            return result

        def start():
            nonlocal server
            server = subprocess.Popen(
                [str(server_bin), f"--listen={endpoint}", f"--db_path={root / 'db'}"],
                stdout=log, stderr=log,
            )
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                check(server.poll() is None, "server exited during startup")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        return
                except OSError:
                    time.sleep(0.05)
            raise AssertionError("server startup timed out")

        try:
            start()
            occupied = subprocess.run(
                [str(server_bin), f"--listen={endpoint}", f"--db_path={root / 'other-db'}",
                 "--ttl_sweep_interval_ms=60000"],
                capture_output=True, timeout=5,
            )
            check(occupied.returncode != 0, "occupied port fails and stops cleanup worker promptly")
            call("put", "ttl", "temporary", extra=("--ttl_ms=100",))
            time.sleep(0.15)
            call("get", "ttl", expected=2)
            call("put", "ttl-reset", "old", extra=("--ttl_ms=100",))
            call("put", "ttl-reset", "permanent")
            time.sleep(0.15)
            check(call("get", "ttl-reset").stdout == b"permanent\n", "overwrite clears TTL")
            call("put", "ttl-invalid", "x", extra=("--ttl_ms=-1",), expected=3)
            call("get", "ttl-reset", extra=("--ttl_ms=100",), expected=3)
            check(call("put", "name", "MiniKV").stdout == b"OK\n", "put output")
            check(call("get", "name").stdout == b"MiniKV\n", "get output")
            with open("/dev/full", "wb") as full:
                failed_output = subprocess.run(
                    [str(client_bin), f"--server={endpoint}", "get", "name"],
                    stdout=full, stderr=subprocess.PIPE, timeout=10,
                )
            check(failed_output.returncode == 1, "stdout write failure must not report success")
            call("put", "name", "updated")
            check(call("get", "name").stdout == b"updated\n", "overwrite")
            call("put", "empty", "")
            check(call("get", "empty").stdout == b"\n", "empty value")
            call("delete", "name")
            call("delete", "name")
            check(b"NOT_FOUND" in call("get", "name", expected=2).stderr, "not found status")
            call("get", "", expected=3)
            call("put", "x" * 1025, "x", expected=3)
            call("unknown", "key", expected=3)

            source = root / "input.bin"
            destination = root / "output.bin"
            payload = bytes(range(256)) * 4096
            source.write_bytes(payload)
            call("put", "binary", extra=(f"--value_file={source}",))
            call("get", "binary", extra=(f"--output_file={destination}",))
            check(destination.read_bytes() == payload, "1 MiB binary round trip")
            source.write_bytes(payload + b"x")
            call("put", "too-big", extra=(f"--value_file={source}",), expected=3)

            def concurrent_roundtrip(index):
                key = f"parallel-{index}"
                call("put", key, key)
                check(call("get", key).stdout == (key + "\n").encode(), "parallel get")

            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
                list(executor.map(concurrent_roundtrip, range(12)))

            call("put", "survives", "acknowledged")
            call("put", "expires-offline", "temporary", extra=("--ttl_ms=100",))
            server.kill()
            server.wait(timeout=5)
            time.sleep(0.15)
            start()
            call("get", "expires-offline", expected=2)
            check(call("get", "survives").stdout == b"acknowledged\n", "SIGKILL recovery")
            call("get", "name", expected=2)
            call("get", "binary", extra=(f"--output_file={destination}",))
            check(destination.read_bytes() == payload, "persistent binary")
            server.terminate()
            check(server.wait(timeout=10) == 0, "graceful shutdown")
            call("get", "survives", expected=1)
            print("PASS: RPC CRUD, TTL, errors, binary limits, concurrency, SIGKILL recovery, shutdown")
        except Exception:
            log.flush()
            print((root / "server.log").read_text(errors="replace")[-12000:], file=sys.stderr)
            raise
        finally:
            if server is not None and server.poll() is None:
                server.kill()
                server.wait(timeout=5)
            log.close()


if __name__ == "__main__":
    main()
