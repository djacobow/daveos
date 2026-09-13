#pragma once

#include <array>
#include <cstddef>
#include <utility>

#include "core/platform/platform.hpp"

namespace daveos::core {


// Fixed-capacity FIFO with no synchronization or dynamic storage.
// T must be default constructible and copy assignable. For embedded/ISR use,
// those operations must not allocate or throw, and must complete promptly.
template <typename T, std::size_t Capacity>
class Queue {
  static_assert(Capacity > 0);

 public:
  // Copy to the tail; full leaves existing entries unchanged.
  Status push(const T& item) {
    if (full()) return Status::full;
    items_[(head_ + count_) % Capacity] = item;
    ++count_;
    return Status::ok;
  }
  // Copy and remove the head; empty leaves the output untouched.
  Status pop(T& item) {
    if (empty()) return Status::empty;
    item = items_[head_];
    head_ = (head_ + 1) % Capacity;
    --count_;
    return Status::ok;
  }
  // Copy the head without removing it; empty leaves the output untouched.
  Status peek(T& item) const {
    if (empty()) return Status::empty;
    item = items_[head_];
    return Status::ok;
  }
  // Forget queued entries without destroying/resetting the backing objects.
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

// A query value is meaningful only when status is ok (or bool(result) is true).
template <typename T>
struct Result {
  Status status;
  T value{};
  explicit operator bool() const { return status == Status::ok; }
};

// FIFO usable from callbacks and interrupts. Prefer the platform's optional
// mutex; fall back to critical sections only when no mutex is provided.
// Mutex acquisition is a single try_lock(): busy leaves queue/output unchanged.
// State queries can also return busy, so their value must not be used on
// failure. The platform must outlive the queue. T has the same constraints as
// Queue.
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
