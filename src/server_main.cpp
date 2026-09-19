// SPDX-License-Identifier: Apache-2.0

#include "kv_service.h"

#include <brpc/server.h>
#include <brpc/controller.h>
#include <gflags/gflags.h>
#include <filesystem>
#include <iostream>

DEFINE_string(listen, "127.0.0.1:18081", "RPC listen endpoint");
DEFINE_string(db_path, "./minikv-data", "RocksDB directory");
DEFINE_bool(sync_writes, true, "Sync the WAL before acknowledging Put/Delete");
DEFINE_int32(worker_threads, 0, "bRPC worker thread hint; 0 keeps the framework default");
DEFINE_int32(max_concurrency, 64, "bRPC built-in concurrent request limit");

int main(int argc, char** argv) {
    gflags::SetUsageMessage("MiniKV single-node persistent KV server");
    gflags::SetCommandLineOption("graceful_quit_on_sigterm", "true");
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    if (argc != 1 || FLAGS_worker_threads < 0 || FLAGS_max_concurrency < 1 || FLAGS_db_path.empty()) {
        std::cerr << "Invalid arguments: nonnegative thread hint, positive concurrency and a DB path are required\n";
        return 1;
    }
    std::error_code error;
    std::filesystem::create_directories(FLAGS_db_path, error);
    if (error) {
        std::cerr << "Create DB directory: " << error.message() << '\n';
        return 1;
    }
    minikv::Store store;
    const auto status = store.Open(FLAGS_db_path, FLAGS_sync_writes);
    if (!status.ok()) {
        std::cerr << "Open database: " << status.ToString() << '\n';
        return 1;
    }

    // Declaration order keeps the service and DB alive until Server is destroyed.
    minikv::KVServiceImpl service(store);
    brpc::Server server;
    if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) return 1;
    brpc::ServerOptions options;
    options.num_threads = FLAGS_worker_threads;
    options.max_concurrency = FLAGS_max_concurrency;
    // Install bRPC's quit handlers before the listening socket becomes visible.
    brpc::IsAskedToQuit();
    if (server.Start(FLAGS_listen.c_str(), &options) != 0) return 1;
    std::cout << "MiniKV listening on " << FLAGS_listen
              << ", db=" << std::filesystem::absolute(FLAGS_db_path)
              << ", sync_writes=" << std::boolalpha << FLAGS_sync_writes << std::endl;
    server.RunUntilAskedToQuit();
    return 0;
}
