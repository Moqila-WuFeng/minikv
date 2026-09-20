// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "store.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace minikv {

// Owns only the cleanup thread; Store must outlive this object.
class ExpiryWorker {
public:
    ExpiryWorker(Store& store, int interval_ms, std::size_t batch_size)
        : thread_([this, &store, interval_ms, batch_size] {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!wake_.wait_for(lock, std::chrono::milliseconds(interval_ms), [this] { return stop_; })) {
                lock.unlock();
                std::size_t removed;
                const auto status = store.SweepExpired(batch_size, &removed);
                if (!status.ok()) std::cerr << "TTL cleanup: " << status.ToString() << '\n';
                lock.lock();
            }
        }) {}

    ~ExpiryWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace minikv
