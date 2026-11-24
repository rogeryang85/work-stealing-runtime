#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

// Chase-Lev deque, memory orderings from Le et al., PPoPP 2013.
template <typename T>
class Deque {
 public:
  Deque(int64_t capacity = 1024) {
    array = new Array(capacity);
  }

  ~Deque() {
    delete array.load();
    for (size_t i = 0; i < old_arrays.size(); i++) {
      delete old_arrays[i];
    }
  }

  void push(T x) {
    int64_t b = bottom.load(std::memory_order_relaxed);
    int64_t t = top.load(std::memory_order_acquire);
    Array* a = array.load(std::memory_order_relaxed);
    if (b - t > a->capacity - 1) {
      a = grow(a, t, b);
    }
    a->put(b, x);
    std::atomic_thread_fence(std::memory_order_release);
    bottom.store(b + 1, std::memory_order_relaxed);
  }

  T pop() {
    int64_t b = bottom.load(std::memory_order_relaxed) - 1;
    Array* a = array.load(std::memory_order_relaxed);
    bottom.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t t = top.load(std::memory_order_relaxed);

    if (t > b) {
      bottom.store(b + 1, std::memory_order_relaxed);
      return nullptr;
    }

    T x = a->get(b);
    if (t == b) {
      // Last element: a thief may be trying to take it too.
      if (!top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                       std::memory_order_relaxed)) {
        x = nullptr;
      }
      bottom.store(b + 1, std::memory_order_relaxed);
    }
    return x;
  }

  T steal() {
    int64_t t = top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t b = bottom.load(std::memory_order_acquire);
    if (t >= b) {
      return nullptr;
    }

    Array* a = array.load(std::memory_order_acquire);
    T x = a->get(t);
    if (!top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                     std::memory_order_relaxed)) {
      return nullptr;
    }
    return x;
  }

  int64_t size() {
    return bottom.load(std::memory_order_relaxed) - top.load(std::memory_order_relaxed);
  }

 private:
  struct Array {
    int64_t capacity;  // always a power of two
    std::atomic<T>* items;

    Array(int64_t c) {
      capacity = c;
      items = new std::atomic<T>[c];
    }
    ~Array() {
      delete[] items;
    }
    T get(int64_t i) {
      return items[i & (capacity - 1)].load(std::memory_order_relaxed);
    }
    void put(int64_t i, T x) {
      items[i & (capacity - 1)].store(x, std::memory_order_relaxed);
    }
  };

  Array* grow(Array* old, int64_t t, int64_t b) {
    Array* a = new Array(old->capacity * 2);
    for (int64_t i = t; i < b; i++) {
      a->put(i, old->get(i));
    }
    array.store(a, std::memory_order_release);
    // Thieves might still be reading the old array, so free it only at the end.
    old_arrays.push_back(old);
    return a;
  }

  alignas(64) std::atomic<int64_t> top{0};
  alignas(64) std::atomic<int64_t> bottom{0};
  alignas(64) std::atomic<Array*> array;
  std::vector<Array*> old_arrays;
};
