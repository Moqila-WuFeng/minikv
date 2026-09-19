// SPDX-License-Identifier: Apache-2.0

#include "store.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    char name[] = "/tmp/minikv-store-XXXXXX";
    const char* directory = mkdtemp(name);
    if (!directory) return 1;
    const std::filesystem::path path(directory);
    try {
        {
            minikv::Store store;
            Check(store.Open(path.string(), true).ok(), "open");
            std::string value;
            Check(store.Get("missing", &value).IsNotFound(), "missing key");
            Check(store.Put("key", "one").ok(), "put");
            Check(store.Get("key", &value).ok() && value == "one", "get");
            Check(store.Put("key", "two").ok(), "overwrite");
            Check(store.Get("key", &value).ok() && value == "two", "overwritten value");
            Check(store.Put("empty", "").ok(), "empty value write");
            Check(store.Get("empty", &value).ok() && value.empty(), "empty is not missing");
            const std::string binary("a\0b\xff", 4);
            Check(store.Put("binary", binary).ok(), "binary write");
            Check(store.Get("binary", &value).ok() && value == binary, "binary read");
            Check(store.Delete("key").ok(), "delete");
            Check(store.Get("key", &value).IsNotFound(), "deleted key");
            Check(store.Delete("key").ok(), "idempotent delete");
            Check(store.Put("", "x").IsInvalidArgument(), "empty key put");
            Check(store.Get("", &value).IsInvalidArgument(), "empty key get");
            Check(store.Delete("").IsInvalidArgument(), "empty key delete");
            Check(store.Put(std::string(1025, 'k'), "x").IsInvalidArgument(), "oversized key");
            Check(store.Put("big", std::string(1024 * 1024 + 1, 'v')).IsInvalidArgument(), "oversized value");
            const std::string boundary(1024 * 1024, 'v');
            Check(store.Put(std::string(1024, 'k'), boundary).ok(), "exact size limits");
            Check(store.Get(std::string(1024, 'k'), &value).ok() && value == boundary, "boundary round trip");
            minikv::Store second;
            Check(!second.Open(path.string(), true).ok(), "database lock");
            std::atomic<bool> good{true};
            std::vector<std::thread> workers;
            for (int t = 0; t < 4; ++t) {
                workers.emplace_back([&, t] {
                    for (int i = 0; i < 20; ++i) {
                        const auto key = std::to_string(t) + ":" + std::to_string(i);
                        std::string actual;
                        if (!store.Put(key, key).ok() || !store.Get(key, &actual).ok() || actual != key) good = false;
                    }
                });
            }
            for (auto& worker : workers) worker.join();
            Check(good, "concurrent operations");
        }
        {
            minikv::Store reopened;
            Check(reopened.Open(path.string(), true).ok(), "reopen");
            std::string value;
            Check(reopened.Get("binary", &value).ok() && value == std::string("a\0b\xff", 4), "persistent binary");
            Check(reopened.Get("key", &value).IsNotFound(), "persistent deletion");
        }
        std::filesystem::remove_all(path);
        std::cout << "PASS: CRUD, binary/empty values, limits, database lock, concurrency, reopen\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        std::filesystem::remove_all(path);
        return 1;
    }
}
