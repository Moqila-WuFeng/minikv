// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <rocksdb/db.h>

namespace minikv {

inline constexpr std::size_t kMaxKeyBytes = 1024;
inline constexpr std::size_t kMaxValueBytes = 1024 * 1024;

// Open once before starting RPC workers; destroy after they have stopped.
class Store {
public:
    using Clock = std::function<std::int64_t()>;
    explicit Store(Clock clock = {});
    ~Store();
    rocksdb::Status Open(const std::string& path, bool sync_writes);
    rocksdb::Status Put(const std::string& key, const std::string& value, std::int64_t ttl_ms = 0);
    rocksdb::Status Get(const std::string& key, std::string* value);
    rocksdb::Status Delete(const std::string& key);
    rocksdb::Status SweepExpired(std::size_t limit, std::size_t* removed);

private:
    rocksdb::Status ValidateKey(const std::string& key) const;
    rocksdb::Status ReadExpiry(const std::string& key, std::int64_t* expiry);
    rocksdb::Status DeleteLocked(const std::string& key);
    std::mutex& KeyMutex(const std::string& key);
    std::unique_ptr<rocksdb::DB> db_;
    rocksdb::ColumnFamilyHandle* values_ = nullptr;
    rocksdb::ColumnFamilyHandle* expiry_ = nullptr;
    rocksdb::WriteOptions write_options_;
    Clock clock_;
    std::array<std::mutex, 64> key_mutexes_;
    std::mutex sweep_mutex_;
    std::string sweep_cursor_;
};

}  // namespace minikv
