// SPDX-License-Identifier: Apache-2.0

#include "bench_stats.h"
#include "minikv.pb.h"

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <gflags/gflags.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

DEFINE_string(server, "127.0.0.1:18081", "Dedicated benchmark server endpoint");
DEFINE_string(operation, "mixed", "Workload: put, get, mixed");
DEFINE_string(key_prefix, "minikv-bench", "Dedicated key namespace; prefill overwrites prefix:INDEX");
DEFINE_int32(requests, 10000, "Total measured RPC attempts, 1..1000000");
DEFINE_int32(concurrency, 4, "Client threads, 1..256 (clamped to requests)");
DEFINE_int32(keys, 1000, "Keyspace size, 1..1000000");
DEFINE_int32(value_size, 128, "Value bytes, 0..1048576");
DEFINE_int32(warmup, 100, "Unmeasured sequential warmup requests, 0..1000000");
DEFINE_int32(read_percent, 50, "Read probability percent for mixed workload, 0..100");
DEFINE_int32(timeout_ms, 2000, "RPC and connection timeout in milliseconds");
DEFINE_uint32(seed, 1, "Deterministic payload and per-worker random seed");
DEFINE_bool(prefill, true, "Populate every key before warmup; this writes even for get workload");

namespace {
using Clock = std::chrono::steady_clock;
enum class Outcome { Success, RpcError, ApplicationError, ValidationError };

struct WorkerResult {
    int succeeded = 0, rpc_errors = 0, application_errors = 0, validation_errors = 0;
    int gets = 0, puts = 0;
    std::vector<double> latency;
    Clock::time_point finished;
    std::exception_ptr error;
};

bool ValidArguments(int argc) {
    return argc == 1 && FLAGS_requests > 0 && FLAGS_requests <= 1000000 &&
           FLAGS_concurrency > 0 && FLAGS_concurrency <= 256 &&
           FLAGS_keys > 0 && FLAGS_keys <= 1000000 &&
           FLAGS_value_size >= 0 && FLAGS_value_size <= 1048576 &&
           FLAGS_warmup >= 0 && FLAGS_warmup <= 1000000 && FLAGS_timeout_ms > 0 &&
           FLAGS_read_percent >= 0 && FLAGS_read_percent <= 100 &&
           !FLAGS_key_prefix.empty() && FLAGS_key_prefix.size() <= 900 &&
           FLAGS_key_prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-:")
               == std::string::npos &&
           (FLAGS_operation == "put" || FLAGS_operation == "get" || FLAGS_operation == "mixed");
}

bool ChooseGet(std::mt19937& random) {
    if (FLAGS_operation == "get") return true;
    if (FLAGS_operation == "put") return false;
    return std::uniform_int_distribution<int>(0, 99)(random) < FLAGS_read_percent;
}

Outcome Call(minikv::KVService_Stub& stub, bool get, int index, const std::string& value) {
    const std::string key = FLAGS_key_prefix + ":" + std::to_string(index);
    brpc::Controller controller;
    minikv::Response response;
    if (get) {
        minikv::KeyRequest request;
        request.set_key(key);
        stub.Get(&controller, &request, &response, nullptr);
    } else {
        minikv::PutRequest request;
        request.set_key(key);
        request.set_value(value);
        stub.Put(&controller, &request, &response, nullptr);
    }
    if (controller.Failed()) return Outcome::RpcError;
    if (response.code() != minikv::OK) return Outcome::ApplicationError;
    if (get && (!response.has_value() || response.value() != value)) return Outcome::ValidationError;
    return Outcome::Success;
}

int Run() {
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = "baidu_std";
    options.timeout_ms = FLAGS_timeout_ms;
    options.connect_timeout_ms = FLAGS_timeout_ms;
    options.max_retry = 0;
    if (channel.Init(FLAGS_server.c_str(), &options) != 0) {
        std::cerr << "Cannot initialize benchmark channel\n";
        return 1;
    }
    minikv::KVService_Stub stub(&channel);
    std::mt19937 payload_random(FLAGS_seed);
    std::string value(static_cast<std::size_t>(FLAGS_value_size), '\0');
    for (char& byte : value) byte = static_cast<char>(payload_random() & 255);
    if (FLAGS_prefill) {
        for (int i = 0; i < FLAGS_keys; ++i) {
            if (Call(stub, false, i, value) != Outcome::Success) {
                std::cerr << "Prefill failed; no measured report produced\n";
                return 1;
            }
        }
    }
    std::mt19937 warmup_random(FLAGS_seed);
    for (int i = 0; i < FLAGS_warmup; ++i) {
        const bool get = ChooseGet(warmup_random);
        const int index = std::uniform_int_distribution<int>(0, FLAGS_keys - 1)(warmup_random);
        if (Call(stub, get, index, value) != Outcome::Success) {
            std::cerr << "Warmup failed; no measured report produced\n";
            return 1;
        }
    }

    const int workers = std::min(FLAGS_requests, FLAGS_concurrency);
    std::vector<WorkerResult> results(static_cast<std::size_t>(workers));
    for (int i = 0; i < workers; ++i) {
        results[i].latency.resize(FLAGS_requests / workers + (i < FLAGS_requests % workers));
    }
    std::mutex mutex;
    std::condition_variable condition;
    int ready = 0;
    bool start = false, cancel = false;
    std::vector<std::thread> threads;
    threads.reserve(workers);
    try {
        for (int i = 0; i < workers; ++i) {
            threads.emplace_back([&, i] {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ++ready;
                    condition.notify_all();
                    condition.wait(lock, [&] { return start; });
                    if (cancel) return;
                }
                auto& result = results[i];
                try {
                    std::mt19937 random(FLAGS_seed + static_cast<std::uint32_t>(i));
                    for (double& latency : result.latency) {
                        const bool get = ChooseGet(random);
                        const int key = std::uniform_int_distribution<int>(0, FLAGS_keys - 1)(random);
                        if (get) ++result.gets; else ++result.puts;
                        const auto before = Clock::now();
                        const auto outcome = Call(stub, get, key, value);
                        latency = std::chrono::duration<double, std::micro>(Clock::now() - before).count();
                        switch (outcome) {
                            case Outcome::Success: ++result.succeeded; break;
                            case Outcome::RpcError: ++result.rpc_errors; break;
                            case Outcome::ApplicationError: ++result.application_errors; break;
                            case Outcome::ValidationError: ++result.validation_errors; break;
                        }
                    }
                } catch (...) {
                    result.error = std::current_exception();
                }
                result.finished = Clock::now();
            });
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            cancel = true;
            start = true;
        }
        condition.notify_all();
        for (auto& thread : threads) thread.join();
        throw;
    }
    Clock::time_point began;
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return ready == workers; });
        began = Clock::now();
        start = true;
    }
    condition.notify_all();
    for (auto& thread : threads) thread.join();

    WorkerResult total;
    total.latency.reserve(FLAGS_requests);
    auto finished = began;
    for (const auto& result : results) {
        if (result.error) std::rethrow_exception(result.error);
        total.succeeded += result.succeeded;
        total.rpc_errors += result.rpc_errors;
        total.application_errors += result.application_errors;
        total.validation_errors += result.validation_errors;
        total.gets += result.gets;
        total.puts += result.puts;
        total.latency.insert(total.latency.end(), result.latency.begin(), result.latency.end());
        finished = std::max(finished, result.finished);
    }
    const double elapsed = std::chrono::duration<double>(finished - began).count();
    const auto latency = minikv::SummarizeLatencies(std::move(total.latency));
    google::protobuf::Struct report;
    auto& fields = *report.mutable_fields();
    const auto number = [&](const std::string& name, double v) { fields[name].set_number_value(v); };
    number("schema_version", 1);
    number("attempted", FLAGS_requests);
    number("succeeded", total.succeeded);
    number("rpc_errors", total.rpc_errors);
    number("application_errors", total.application_errors);
    number("validation_errors", total.validation_errors);
    number("get_attempted", total.gets);
    number("put_attempted", total.puts);
    number("concurrency", workers);
    number("keys", FLAGS_keys);
    number("value_size", FLAGS_value_size);
    number("seed", FLAGS_seed);
    number("read_percent", FLAGS_read_percent);
    number("timeout_ms", FLAGS_timeout_ms);
    number("prefill_completed", FLAGS_prefill ? FLAGS_keys : 0);
    number("warmup_completed", FLAGS_warmup);
    number("elapsed_seconds", elapsed);
    number("attempt_ops_per_second", FLAGS_requests / elapsed);
    number("success_ops_per_second", total.succeeded / elapsed);
    number("success_rate", static_cast<double>(total.succeeded) / FLAGS_requests);
    fields["operation"].set_string_value(FLAGS_operation);
    fields["key_prefix"].set_string_value(FLAGS_key_prefix);
    fields["server"].set_string_value(FLAGS_server);
    fields["load_model"].set_string_value("closed_loop");
    auto& times = *fields["attempt_latency_us"].mutable_struct_value()->mutable_fields();
    times["samples"].set_number_value(latency.samples);
    times["min"].set_number_value(latency.min);
    times["max"].set_number_value(latency.max);
    times["mean"].set_number_value(latency.mean);
    times["p50"].set_number_value(latency.p50);
    times["p95"].set_number_value(latency.p95);
    times["p99"].set_number_value(latency.p99);
    std::string json;
    const auto status = google::protobuf::util::MessageToJsonString(report, &json);
    if (!status.ok()) {
        std::cerr << "Cannot serialize benchmark report\n";
        return 1;
    }
    std::cout << json << '\n' << std::flush;
    if (!std::cout) {
        std::cerr << "Cannot write benchmark report\n";
        return 1;
    }
    return total.succeeded == FLAGS_requests ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    gflags::SetUsageMessage("MiniKV closed-loop RPC benchmark; use a dedicated database");
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    if (!ValidArguments(argc)) {
        std::cerr << "Invalid benchmark arguments; see --help\n";
        return 2;
    }
    try {
        return Run();
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
