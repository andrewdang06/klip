#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace klip {

template <typename T, std::size_t Capacity>
class SpscRingQueue {
  static_assert(Capacity >= 2, "Capacity must be >= 2");

 public:
  bool try_push(T&& value) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = increment(head);
    if (next == tail_.load(std::memory_order_acquire)) {
      return false;
    }

    buffer_[head].emplace(std::move(value));
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return false;
    }

    out = std::move(*buffer_[tail]);
    buffer_[tail].reset();
    tail_.store(increment(tail), std::memory_order_release);
    return true;
  }

  bool empty() const {
    return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
  }

 private:
  static constexpr std::size_t increment(std::size_t idx) noexcept { return (idx + 1) % Capacity; }

  std::array<std::optional<T>, Capacity> buffer_{};
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
};

template <typename T, std::size_t Capacity>
class MpmcBoundedQueue {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
  static_assert(Capacity >= 2, "Capacity must be >= 2");

  struct Cell {
    std::atomic<std::size_t> sequence;
    alignas(T) unsigned char storage[sizeof(T)];
  };

 public:
  MpmcBoundedQueue() {
    for (std::size_t i = 0; i < Capacity; ++i) {
      buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  ~MpmcBoundedQueue() {
    T item;
    while (try_dequeue(item)) {
    }
  }

  bool try_enqueue(T&& value) {
    Cell* cell = nullptr;
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

    for (;;) {
      cell = &buffer_[pos & mask_];
      const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
      const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }

    new (cell->storage) T(std::move(value));
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }

  bool try_dequeue(T& out) {
    Cell* cell = nullptr;
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

    for (;;) {
      cell = &buffer_[pos & mask_];
      const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
      const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

      if (diff == 0) {
        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }

    T* element = reinterpret_cast<T*>(cell->storage);
    out = std::move(*element);
    element->~T();

    cell->sequence.store(pos + Capacity, std::memory_order_release);
    return true;
  }

 private:
  static constexpr std::size_t mask_ = Capacity - 1;

  std::array<Cell, Capacity> buffer_{};
  alignas(64) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(64) std::atomic<std::size_t> dequeue_pos_{0};
};

}  // namespace klip
