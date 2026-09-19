# SPDX-License-Identifier: Apache-2.0

"""Deterministic regressions for startup ownership and build metadata."""
import argparse
import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("compare_wal", sys.argv.pop(1))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerTest(unittest.TestCase):
    def test_open_port_without_child_readiness_never_receives_writes(self):
        args = argparse.Namespace(build_dir=pathlib.Path("/unused"), server_threads=4,
                                  server_concurrency=64, requests=1, keys=1, warmup=0,
                                  value_size=1, read_percent=50, seed=1, timeout_ms=50,
                                  case_timeout=1)
        process = mock.Mock()
        process.poll.return_value = None
        process.wait.return_value = 0
        result = subprocess.CompletedProcess([], 0, '{"succeeded":1}', "")
        with mock.patch.object(runner.subprocess, "Popen", return_value=process), \
             mock.patch.object(runner.subprocess, "run", return_value=result) as benchmark, \
             mock.patch.object(runner.socket, "create_connection"), \
             mock.patch.object(runner.time, "monotonic", side_effect=[0, 16]):
            with self.assertRaisesRegex(RuntimeError, "startup timed out"):
                runner.run_case(args, "put", 1, True, 1)
            benchmark.assert_not_called()
            process.kill.assert_called_once()

    def test_metadata_uses_build_source_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            build = pathlib.Path(directory)
            source = build / "other-checkout"
            source.mkdir()
            (build / "CMakeCache.txt").write_text(
                f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\nCMAKE_BUILD_TYPE:STRING=Release\n")
            for name in ("minikv_server", "minikv_bench"):
                (build / name).write_bytes(b"fixture")
            with mock.patch.object(runner, "command_text", return_value="fixture") as command:
                runner.environment(build)
            git_calls = [call.args[0] for call in command.call_args_list if call.args[0][0] == "git"]
            self.assertEqual(len(git_calls), 2)
            self.assertTrue(all(call[2] == str(source) for call in git_calls))


if __name__ == "__main__":
    unittest.main()
