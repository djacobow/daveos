#pragma once

#include <array>
#include <cstddef>
#include <utility>

#include "daveos/core/platform.h"

namespace daveos::core {
template <typename T, std::size_t Capacity>
class Queue {
  static_assert(Capacity > 0);

 public:
  Status push(const T& item) {
    if (full()) return Status::full;
    items_[(head_ + count_) % Capacity] = item;
    ++count_;
    return Status::ok;
  }
  Status pop(T& item) {
    if (empty()) return Status::empty;
    item = items_[head_];
    head_ = (head_ + 1) % Capacity;
    --count_;
    return Status::ok;
  }
  Status peek(T& item) const {
    if (empty()) return Status::empty;
    item = items_[head_];
    return Status::ok;
  }
  void clear() { head_ = count_ = 0; }
  std::size_t size() const { return count_; }
  static constexpr std::size_t capacity() { return Capacity; }
  bool empty() const { return count_ == 0; }
  bool full() const { return count_ == Capacity; }

 private:
  std::array<T, Capacity> items_{};
  std::size_t head_ = 0;
  std::size_t count_ = 0;
};

template <typename T>
struct Result {
  Status status;
  T value{};
  explicit operator bool() const { return status == Status::ok; }
};

// Every operation is nonblocking, including state queries. A busy query has no
// value.
template <typename T, std::size_t Capacity, typename P>
class ThreadSafeQueue {
 public:
  explicit ThreadSafeQueue(P& platform)
      : platform_(platform), mutex_(platform.queue_mutex()) {}
  Status push(const T& item) {
    return WithLock([&] { return queue_.push(item); });
  }
  Status pop(T& item) {
    return WithLock([&] { return queue_.pop(item); });
  }
  Status peek(T& item) {
    return WithLock([&] { return queue_.peek(item); });
  }
  Status clear() {
    return WithLock([&] {
      queue_.clear();
      return Status::ok;
    });
  }
  Result<std::size_t> size() {
    return Query<std::size_t>([&] { return queue_.size(); });
  }
  Result<bool> empty() {
    return Query<bool>([&] { return queue_.empty(); });
  }
  Result<bool> full() {
    return Query<bool>([&] { return queue_.full(); });
  }
  static constexpr std::size_t capacity() { return Capacity; }

 private:
  template <typename Function>
  Status WithLock(Function function) {
    if (mutex_) {
      if (!mutex_->try_lock()) return Status::busy;
      Status status = function();
      mutex_->unlock();
      return status;
    }
    Guard guard(platform_);
    return function();
  }
  template <typename Value, typename Function>
  Result<Value> Query(Function function) {
    Result<Value> result{Status::busy};
    result.status = WithLock([&] {
      result.value = function();
      return Status::ok;
    });
    return result;
  }
  P& platform_;
  decltype(std::declval<P&>().queue_mutex()) mutex_;
  Queue<T, Capacity> queue_;
};
}  // namespace daveos::core
