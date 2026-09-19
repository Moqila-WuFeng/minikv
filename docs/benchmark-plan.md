# RPC Benchmark Implementation

Goal: measure the existing RPC service without changing its protocol or storage semantics.

## Design

`minikv_bench` is a C++17 closed-loop client: N native threads share a bRPC
Channel, each sends one synchronous RPC at a time with its own Controller.
Prefill and warmup finish before workers start together. Requests are divided
exactly, including remainders. Each worker keeps private counters and latency
samples; only the start gate is shared. Summaries are merged after joining.

Workloads are random-key Put overwrites, Get, and configurable mixed reads/writes.
All prefilled keys share one deterministic binary payload. Successful reads
must match it. Automatic retries remain disabled. Exact nearest-rank latency
percentiles include failed attempts; throughput reports both attempted and
successful operations. JSON output uses the existing Protobuf JSON serializer.
One million requests and 256 worker threads are explicit upper bounds.

A Python comparison runner launches fresh temporary databases on loopback,
alternates WAL sync on/off, records configuration and environment, and always
stops its child processes. It never uses the normal service database. Each
subprocess has a deadline. Reports are local artifacts, not capacity guarantees.

## Steps

- [x] Add black-box tests for JSON counts, prefill exclusion, read validation,
  workload mix endpoints, invalid arguments, unreachable service and stdout failure.
  Run the test against the absent benchmark and observe the missing-feature failure.
- [x] Add the benchmark executable and deterministic percentile unit tests.
  Build and run all CTest groups until counts, percentiles and failure paths pass.
- [x] Add a temporary-database comparison runner and its integration test.
  Exercise both sync policies, then collect a small Release-build experiment.
- [ ] Document methodology, upstream inspirations and limits; review the diff,
  rerun tests, commit and publish the verified update.

## Sources

- Apache bRPC 1.18.0 `example/echo_c++/client.cpp`, commit
  `94f1bbd32845a45f0218dfe43e25791252bdcb72`: Channel/Stub lifetime and Controller use.
- https://github.com/facebook/rocksdb/wiki/Benchmarking-tools: separating setup
  from read/overwrite workloads and recording benchmark options.

These are design references, not copied source implementations. Existing
upstream licenses and notices remain in `third_party/licenses/`.
