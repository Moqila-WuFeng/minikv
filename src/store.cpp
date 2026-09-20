// SPDX-License-Identifier: Apache-2.0

#include "store.h"

#include <charconv>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>
#include <rocksdb/write_batch.h>

namespace minikv {

Store::Store(Clock clock) : clock_(std::move(clock)) {
    if (!clock_) clock_ = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    };
}

Store::~Store() {
    // Column-family handles must be destroyed before the database.
    if (expiry_) db_->DestroyColumnFamilyHandle(expiry_);
    if (values_) db_->DestroyColumnFamilyHandle(values_);
}

rocksdb::Status Store::Open(const std::string& path, bool sync_writes) {
    if (db_) return rocksdb::Status::InvalidArgument("database already open");
    rocksdb::Options options;
    options.create_if_missing = true;
    options.create_missing_column_families = true;
    const std::vector<rocksdb::ColumnFamilyDescriptor> families{
        {rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions{}},
        {"minikv_expiry_v1", rocksdb::ColumnFamilyOptions{}}};
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    rocksdb::DB* database = nullptr;
    const auto status = rocksdb::DB::Open(options, path, families, &handles, &database);
    if (status.ok()) {
        db_.reset(database);
        values_ = handles[0];
        expiry_ = handles[1];
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

std::mutex& Store::KeyMutex(const std::string& key) {
    return key_mutexes_[std::hash<std::string>{}(key) % key_mutexes_.size()];
}

rocksdb::Status Store::ReadExpiry(const std::string& key, std::int64_t* expiry) {
    *expiry = 0;
    std::string encoded;
    const auto status = db_->Get(rocksdb::ReadOptions{}, expiry_, key, &encoded);
    if (status.IsNotFound()) return rocksdb::Status::OK();
    if (!status.ok()) return status;
    const auto result = std::from_chars(encoded.data(), encoded.data() + encoded.size(), *expiry);
    if (result.ec != std::errc{} || result.ptr != encoded.data() + encoded.size() || *expiry <= 0) {
        return rocksdb::Status::Corruption("invalid expiry metadata");
    }
    return rocksdb::Status::OK();
}

rocksdb::Status Store::Put(const std::string& key, const std::string& value, std::int64_t ttl_ms) {
    const auto status = ValidateKey(key);
    if (!status.ok()) return status;
    if (value.size() > kMaxValueBytes) {
        return rocksdb::Status::InvalidArgument("value exceeds 1048576 bytes");
    }
    if (ttl_ms < 0) return rocksdb::Status::InvalidArgument("TTL must be nonnegative");
    std::lock_guard<std::mutex> lock(KeyMutex(key));
    const auto now = clock_();
    if (ttl_ms > 0 && (now < 0 || ttl_ms > std::numeric_limits<std::int64_t>::max() - now)) {
        return rocksdb::Status::InvalidArgument("TTL deadline out of range");
    }
    rocksdb::WriteBatch batch;
    batch.Put(values_, key, value);
    if (ttl_ms == 0) batch.Delete(expiry_, key);
    else batch.Put(expiry_, key, std::to_string(now + ttl_ms));
    return db_->Write(write_options_, &batch);
}

rocksdb::Status Store::Get(const std::string& key, std::string* value) {
    const auto status = ValidateKey(key);
    if (!status.ok()) return status;
    if (!value) return rocksdb::Status::InvalidArgument("null output");
    value->clear();
    std::lock_guard<std::mutex> lock(KeyMutex(key));
    std::int64_t expiry;
    const auto expiry_status = ReadExpiry(key, &expiry);
    if (!expiry_status.ok()) return expiry_status;
    if (expiry != 0 && clock_() >= expiry) return rocksdb::Status::NotFound();
    return db_->Get(rocksdb::ReadOptions{}, values_, key, value);
}

rocksdb::Status Store::Delete(const std::string& key) {
    const auto status = ValidateKey(key);
    if (!status.ok()) return status;
    std::lock_guard<std::mutex> lock(KeyMutex(key));
    return DeleteLocked(key);
}

rocksdb::Status Store::DeleteLocked(const std::string& key) {
    rocksdb::WriteBatch batch;
    batch.Delete(values_, key);
    batch.Delete(expiry_, key);
    return db_->Write(write_options_, &batch);
}

rocksdb::Status Store::SweepExpired(std::size_t limit, std::size_t* removed) {
    if (!db_) return rocksdb::Status::IOError("database is not open");
    if (!removed || limit == 0) return rocksdb::Status::InvalidArgument("positive sweep limit and output required");
    *removed = 0;
    std::lock_guard<std::mutex> sweep_lock(sweep_mutex_);
    rocksdb::ReadOptions options;
    options.fill_cache = false;
    std::unique_ptr<rocksdb::Iterator> iterator(db_->NewIterator(options, expiry_));
    if (sweep_cursor_.empty()) iterator->SeekToFirst();
    else {
        iterator->Seek(sweep_cursor_);
        if (iterator->Valid() && iterator->key().ToString() == sweep_cursor_) iterator->Next();
    }
    for (std::size_t scanned = 0; iterator->Valid() && scanned < limit; ++scanned, iterator->Next()) {
        const auto key = iterator->key().ToString();
        // The iterator may be stale: re-read under the same lock used by Put/Delete.
        std::lock_guard<std::mutex> lock(KeyMutex(key));
        // An unreadable entry is reported, but must not starve later keys forever.
        sweep_cursor_ = key;
        std::int64_t expiry;
        auto status = ReadExpiry(key, &expiry);
        if (!status.ok()) return status;
        if (expiry != 0 && clock_() >= expiry) {
            status = DeleteLocked(key);
            if (!status.ok()) return status;
            ++*removed;
        }
    }
    if (!iterator->status().ok()) return iterator->status();
    if (!iterator->Valid()) sweep_cursor_.clear();
    return rocksdb::Status::OK();
}

}  // namespace minikv
