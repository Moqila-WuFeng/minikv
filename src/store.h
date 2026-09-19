// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <rocksdb/db.h>

namespace minikv {

inline constexpr std::size_t kMaxKeyBytes = 1024;
inline constexpr std::size_t kMaxValueBytes = 1024 * 1024;

// Open once before starting RPC workers; destroy after they have stopped.
class Store {
public:
    rocksdb::Status Open(const std::string& path, bool sync_writes);
    rocksdb::Status Put(const std::string& key, const std::string& value);
    rocksdb::Status Get(const std::string& key, std::string* value);
    rocksdb::Status Delete(const std::string& key);

private:
    rocksdb::Status ValidateKey(const std::string& key) const;
    std::unique_ptr<rocksdb::DB> db_;
    rocksdb::WriteOptions write_options_;
};

}  // namespace minikv
