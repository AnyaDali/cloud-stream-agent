#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace cloud_stream::pipeline {

template <typename Value> class BoundedLatestQueue {
public:
    explicit BoundedLatestQueue(const std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("queue capacity must be positive");
        }
    }

    BoundedLatestQueue(const BoundedLatestQueue&) = delete;
    BoundedLatestQueue& operator=(const BoundedLatestQueue&) = delete;

    bool push(Value value) {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return false;
        }
        if (values_.size() == capacity_) {
            values_.pop_front();
            ++dropped_count_;
        }
        values_.push_back(std::move(value));
        condition_.notify_one();
        return true;
    }

    bool wait_pop(Value& value) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return closed_ || !values_.empty(); });
        if (values_.empty()) {
            return false;
        }
        value = std::move(values_.front());
        values_.pop_front();
        return true;
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] std::uint64_t dropped_count() const {
        std::lock_guard lock(mutex_);
        return dropped_count_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Value> values_;
    std::uint64_t dropped_count_{0};
    bool closed_{false};
};

} // namespace cloud_stream::pipeline
