// SPDX-License-Identifier: Apache-2.0

#include "store.h"
#include "expiry_worker.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <vector>

void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void CheckPhysicalDeletion(const std::filesystem::path& path, const std::string& key) {
    std::vector<rocksdb::ColumnFamilyDescriptor> families{
        {rocksdb::kDefaultColumnFamilyName, {}}, {"minikv_expiry_v1", {}}};
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    rocksdb::DB* raw = nullptr;
    Check(rocksdb::DB::Open({}, path.string(), families, &handles, &raw).ok(), "inspect DB");
    std::unique_ptr<rocksdb::DB> db(raw);
    std::string value;
    const bool values_missing = db->Get({}, handles[0], key, &value).IsNotFound();
    const bool metadata_missing = db->Get({}, handles[1], key, &value).IsNotFound();
    for (auto* handle : handles) db->DestroyColumnFamilyHandle(handle);
    Check(values_missing && metadata_missing, "both column families logically deleted");
}

void CheckStaleIterator(const std::filesystem::path& path) {
    std::atomic<std::int64_t> now{1000};
    std::atomic<bool> pause{false};
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, resume = false;
    minikv::Store store([&] {
        if (pause.exchange(false)) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return resume; });
        }
        return now.load();
    });
    Check(store.Open(path.string(), true).ok(), "race DB");
    const std::string first = "a-block";
    std::string last = "z-racing";
    while (std::hash<std::string>{}(first) % 64 == std::hash<std::string>{}(last) % 64) last += 'z';
    Check(store.Put(first, "old", 1).ok() && store.Put(last, "old", 1).ok(), "race setup");
    now = 1002;
    pause = true;
    rocksdb::Status sweep_status;
    std::size_t removed = 0;
    std::thread sweeper([&] { sweep_status = store.SweepExpired(100, &removed); });
    bool ready;
    {
        std::unique_lock<std::mutex> lock(mutex);
        ready = cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    // Sweep already owns a snapshot containing last's expired metadata.
    const auto put_status = store.Put(last, "fresh");
    {
        std::lock_guard<std::mutex> lock(mutex);
        resume = true;
    }
    cv.notify_all();
    sweeper.join();
    Check(ready && put_status.ok() && sweep_status.ok() && removed == 1, "stale snapshot rechecked");
    std::string value;
    Check(store.Get(last, &value).ok() && value == "fresh", "new value survives stale cleanup candidate");
}

void CheckWorker(const std::filesystem::path& path) {
    std::atomic<std::int64_t> now{1000};
    std::atomic<bool> pause{false};
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, resume = false;
    {
        minikv::Store store([&] {
            if (pause.exchange(false)) {
                std::unique_lock<std::mutex> lock(mutex);
                entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return resume; });
            }
            return now.load();
        });
        Check(store.Open(path.string(), true).ok(), "worker DB");
        Check(store.Put("expired", "value", 1).ok(), "worker setup");
        now = 1002;
        pause = true;
        auto worker = std::make_unique<minikv::ExpiryWorker>(store, 1, 10);
        bool ready;
        {
            std::unique_lock<std::mutex> lock(mutex);
            ready = cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
        }
        std::thread shutdown([&] { worker.reset(); });
        {
            std::lock_guard<std::mutex> lock(mutex);
            resume = true;
        }
        cv.notify_all();
        shutdown.join();
        Check(ready, "worker actually entered cleanup");
        const auto start = std::chrono::steady_clock::now();
        { minikv::ExpiryWorker waiting(store, 60000, 10); }
        Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "shutdown wakes long wait");
    }
    CheckPhysicalDeletion(path, "expired");
}

int main() {
    char name[] = "/tmp/minikv-ttl-XXXXXX";
    if (!mkdtemp(name)) return 1;
    const std::filesystem::path root(name);
    try {
        // A real pre-TTL database: default column family, unmodified binary values.
        {
            rocksdb::Options options;
            options.create_if_missing = true;
            rocksdb::DB* raw = nullptr;
            Check(rocksdb::DB::Open(options, root.string(), &raw).ok(), "legacy open");
            std::unique_ptr<rocksdb::DB> legacy(raw);
            Check(legacy->Put({}, "legacy", std::string("\0old\xff", 5)).ok(), "legacy write");
        }
        std::atomic<std::int64_t> now{1000};
        {
            minikv::Store store([&] { return now.load(); });
            Check(store.Open(root.string(), true).ok(), "open upgrade");
            std::string value;
            Check(store.Get("legacy", &value).ok() && value == std::string("\0old\xff", 5), "legacy binary intact");
            Check(store.Put("key", "live", 10).ok(), "ttl write");
            now = 1009;
            Check(store.Get("key", &value).ok() && value == "live", "before deadline");
            now = 1010;
            Check(store.Get("key", &value).IsNotFound() && value.empty(), "exact deadline");
            now = 1009;
            Check(store.Get("key", &value).ok(), "wall clock rollback semantics before cleanup");
            now = 1010;
            Check(store.Put("key", "old", 10).ok(), "expiring overwrite");
            Check(store.Put("key", "permanent").ok(), "clear TTL");
            now = 2000;
            Check(store.Get("key", &value).ok() && value == "permanent", "no stale expiry");
            Check(store.Put("key", "bad", -1).IsInvalidArgument(), "negative TTL");
            Check(store.Put("key", "bad", std::numeric_limits<std::int64_t>::max()).IsInvalidArgument(), "overflow TTL");
            Check(store.Get("key", &value).ok() && value == "permanent", "invalid put unchanged");
            Check(store.Put("deleted", "x", 1).ok(), "delete TTL put");
            Check(store.Delete("deleted").ok(), "delete TTL");
            Check(store.Put("deleted", "new").ok(), "reinsert");
            Check(store.Put("offline", "x", 100).ok(), "persistent deadline");
            Check(store.Put("refresh", "old", 1).ok(), "refresh first");
            Check(store.Put("refresh", "new", 10000).ok(), "refresh deadline");
            for (int i = 0; i < 9; ++i) {
                Check(store.Put("expired-" + std::to_string(i), "x", 1).ok(), "sweep setup");
            }
            now = 2002;
            std::size_t total = 0;
            for (int i = 0; i < 20; ++i) {
                std::size_t removed = 999;
                Check(store.SweepExpired(2, &removed).ok() && removed <= 2, "bounded sweep");
                total += removed;
            }
            Check(total == 9, "cursor visits all expired keys");
            Check(store.Get("refresh", &value).ok() && value == "new", "sweep preserves refreshed value");
            Check(store.SweepExpired(0, &total).IsInvalidArgument(), "zero sweep limit");
            Check(store.SweepExpired(1, nullptr).IsInvalidArgument(), "null sweep output");
            std::atomic<bool> good{true};
            std::thread writer([&] {
                for (int i = 0; i < 500; ++i) {
                    if (!store.Put("racing", "fresh", 10000).ok()) good = false;
                }
            });
            std::thread cleaner([&] {
                for (int i = 0; i < 500; ++i) {
                    std::size_t removed;
                    if (!store.SweepExpired(2, &removed).ok()) good = false;
                }
            });
            writer.join();
            cleaner.join();
            Check(good && store.Get("racing", &value).ok() && value == "fresh", "concurrent cleanup and writes");
        }
        now = 2100;
        CheckPhysicalDeletion(root, "expired-8");
        {
            minikv::Store reopened([&] { return now.load(); });
            Check(reopened.Open(root.string(), true).ok(), "reopen TTL database");
            std::string value;
            Check(reopened.Get("offline", &value).IsNotFound(), "expiry survives restart");
            Check(reopened.Get("deleted", &value).ok() && value == "new", "deleted metadata stays deleted");
            Check(reopened.Get("legacy", &value).ok(), "legacy remains persistent");
        }
        CheckStaleIterator(root / "race");
        CheckWorker(root / "worker");
        {
            std::vector<rocksdb::ColumnFamilyDescriptor> families{
                {rocksdb::kDefaultColumnFamilyName, {}}, {"minikv_expiry_v1", {}}};
            std::vector<rocksdb::ColumnFamilyHandle*> handles;
            rocksdb::DB* raw = nullptr;
            Check(rocksdb::DB::Open({}, root.string(), families, &handles, &raw).ok(), "open metadata fixture");
            std::unique_ptr<rocksdb::DB> db(raw);
            const auto bad = db->Put({}, handles[1], "a-corrupt", "invalid");
            for (auto* handle : handles) db->DestroyColumnFamilyHandle(handle);
            Check(bad.ok(), "inject corrupt metadata");
        }
        {
            minikv::Store store([&] { return now.load(); });
            Check(store.Open(root.string(), true).ok(), "open corruption fixture");
            std::string value;
            Check(store.Get("a-corrupt", &value).IsCorruption(), "corrupt TTL not treated as permanent");
            Check(store.Put("z-expired", "x", 1).ok(), "healthy key behind corruption");
            now = 3000;
            std::size_t removed;
            Check(store.SweepExpired(100, &removed).IsCorruption(), "cleanup reports corruption");
            Check(store.SweepExpired(100, &removed).ok() && removed >= 1, "cleanup progresses past corruption");
        }
        CheckPhysicalDeletion(root, "z-expired");
        std::filesystem::remove_all(root);
        std::cout << "PASS: TTL boundaries, migration, restart, atomic metadata, bounded cleanup, concurrency\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
