// SPDX-License-Identifier: Apache-2.0

#include "store.h"

namespace minikv {

rocksdb::Status Store::Open(const std::string& path, bool sync_writes) {
    if (db_) return rocksdb::Status::InvalidArgument("database already open");
    rocksdb::Options options;
    options.create_if_missing = true;
    rocksdb::DB* database = nullptr;
    const auto status = rocksdb::DB::Open(options, path, &database);
    if (status.ok()) {
        db_.reset(database);
        write_options_.sync = sync_writes;
        write_options_.disableWAL = false;
    }
    return status;
}

rocksdb::Status Store::ValidateKey(const std::string& key) const {
    if (!db_) return rocksdb::Status::IOError("database is not open");
    if (key.empty() || key.size() > kMaxKeyBytes) {
        return rocksdb::Status::InvalidArgument("key must contain 1..1024 bytes");
    }
    return rocksdb::Status::OK();
}

rocksdb::Status Store::Put(const std::string& key, const std::string& value) {
    const auto status = ValidateKey(key);
    if (!status.ok()) return status;
    if (value.size() > kMaxValueBytes) {
        return rocksdb::Status::InvalidArgument("value exceeds 1048576 bytes");
    }
    return db_->Put(write_options_, key, value);
}

rocksdb::Status Store::Get(const std::string& key, std::string* value) {
    const auto status = ValidateKey(key);
    if (!status.ok()) return status;
    if (!value) return rocksdb::Status::InvalidArgument("null output");
    value->clear();
    return db_->Get(rocksdb::ReadOptions{}, key, value);
}

rocksdb::Status Store::Delete(const std::string& key) {
    const auto status = ValidateKey(key);
    if (!status.ok()) return status;
    return db_->Delete(write_options_, key);
}

}  // namespace minikv
