// SPDX-License-Identifier: Apache-2.0

#include "minikv.pb.h"

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <gflags/gflags.h>
#include <fstream>
#include <iostream>
#include <string>

DEFINE_string(server, "127.0.0.1:18081", "MiniKV server endpoint");
DEFINE_int32(timeout_ms, 2000, "Per-call timeout in milliseconds");
DEFINE_int64(ttl_ms, 0, "Put lifetime in milliseconds; 0 means no expiration");
DEFINE_string(value_file, "", "Read Put value from a binary file");
DEFINE_string(output_file, "", "Write Get value to a binary file, without newline");

namespace {
int Usage() {
    std::cerr << "Usage: minikv_cli [flags] put KEY VALUE | get KEY | delete KEY\n"
              << "       minikv_cli --value_file=FILE put KEY\n"
              << "       minikv_cli --output_file=FILE get KEY\n";
    return 3;
}
}  // namespace

int main(int argc, char** argv) {
    gflags::SetUsageMessage("MiniKV command-line client; use --help for flags");
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    if (argc < 3 || FLAGS_timeout_ms < 1 || FLAGS_ttl_ms < 0) return Usage();
    const std::string command = argv[1];
    if (command != "put" && command != "get" && command != "delete") return Usage();
    if (command != "put" && FLAGS_ttl_ms != 0) return Usage();
    const int expected = command == "put" && FLAGS_value_file.empty() ? 4 : 3;
    if (argc != expected || (!FLAGS_value_file.empty() && command != "put") ||
        (!FLAGS_output_file.empty() && command != "get")) return Usage();

    std::string value;
    if (command == "put") {
        if (FLAGS_value_file.empty()) {
            value = argv[3];
        } else {
            std::ifstream input(FLAGS_value_file, std::ios::binary);
            if (!input) {
                std::cerr << "Cannot open input file\n";
                return 3;
            }
            value.resize(1024 * 1024 + 1);
            input.read(value.data(), static_cast<std::streamsize>(value.size()));
            value.resize(static_cast<std::size_t>(input.gcount()));
            if (input.bad() || value.size() > 1024 * 1024) {
                std::cerr << "Input read failed or value exceeds 1048576 bytes\n";
                return 3;
            }
        }
    }

    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = "baidu_std";
    options.timeout_ms = FLAGS_timeout_ms;
    options.connect_timeout_ms = FLAGS_timeout_ms;
    // A timeout can mean the write committed but its response was lost.
    options.max_retry = 0;
    if (channel.Init(FLAGS_server.c_str(), &options) != 0) {
        std::cerr << "Cannot initialize RPC channel\n";
        return 1;
    }
    minikv::KVService_Stub stub(&channel);
    brpc::Controller controller;
    minikv::Response response;
    if (command == "put") {
        minikv::PutRequest request;
        request.set_key(argv[2]);
        request.set_value(value);
        request.set_ttl_ms(FLAGS_ttl_ms);
        stub.Put(&controller, &request, &response, nullptr);
    } else {
        minikv::KeyRequest request;
        request.set_key(argv[2]);
        if (command == "get") stub.Get(&controller, &request, &response, nullptr);
        else stub.Delete(&controller, &request, &response, nullptr);
    }
    if (controller.Failed()) {
        std::cerr << "RPC_ERROR: " << controller.ErrorText() << '\n';
        return 1;
    }
    if (response.code() != minikv::OK) {
        std::cerr << minikv::StatusCode_Name(response.code()) << ": " << response.message() << '\n';
        if (response.code() == minikv::NOT_FOUND) return 2;
        if (response.code() == minikv::INVALID_ARGUMENT) return 3;
        return 4;
    }
    if (command == "get") {
        if (FLAGS_output_file.empty()) {
            std::cout.write(response.value().data(), static_cast<std::streamsize>(response.value().size()));
            std::cout << '\n';
        } else {
            std::ofstream output(FLAGS_output_file, std::ios::binary);
            output.write(response.value().data(), static_cast<std::streamsize>(response.value().size()));
            output.close();
            if (!output) {
                std::cerr << "Cannot write output file\n";
                return 3;
            }
        }
    } else {
        std::cout << "OK\n";
    }
    std::cout.flush();
    if (!std::cout) {
        std::cerr << "Cannot write standard output\n";
        return 1;
    }
    return 0;
}
