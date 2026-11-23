// Chase-Lev work-stealing deque, using the C11/C++11 memory orderings from
// Le, Pop, Cohen, Zappa Nardelli, "Correct and Efficient Work-Stealing for
// Weak Memory Models" (PPoPP 2013), Fig. 1.
//
// The owner calls push/pop on the bottom; any thread may call steal on the top.
// Elements are pointers (nullptr is reserved to mean "empty").
#pragma once
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace ws {

template <typename T>
class Deque {
  static_assert(std::is_pointer<T>::value, "Deque holds pointers");

  struct Array {
    int64_t cap;  // always a power of two
    std::atomic<T>* buf;
    explicit Array(int64_t c) : cap(c), buf(new std::atomic<T>[c]) {}
    ~Array() { delete[] buf; }
    T get(int64_t i) const { return buf[i & (cap - 1)].load(std::memory_order_relaxed); }
    void put(int64_t i, T x) { buf[i & (cap - 1)].store(x, std::memory_order_relaxed); }
  };

 public:
  // Result of a steal: an element, empty, or lost a race (caller may retry).
  enum class StealResult { Ok, Empty, Abort };

  explicit Deque(int64_t initial_cap = 1024) : array_(new Array(initial_cap)) {}

  ~Deque() {
    delete array_.load(std::memory_order_relaxed);
    for (Array* a : garbage_) delete a;
  }

  Deque(const Deque&) = delete;
  Deque& operator=(const Deque&) = delete;

  // Owner only.
  void push(T x) {
    int64_t b = bottom_.load(std::memory_order_relaxed);
    int64_t t = top_.load(std::memory_order_acquire);
    Array* a = array_.load(std::memory_order_relaxed);
    if (b - t > a->cap - 1) a = grow(a, t, b);
    a->put(b, x);
    std::atomic_thread_fence(std::memory_order_release);
    bottom_.store(b + 1, std::memory_order_relaxed);
  }

  // Owner only. Returns nullptr if empty.
  T pop() {
    int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    Array* a = array_.load(std::memory_order_relaxed);
    bottom_.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t t = top_.load(std::memory_order_relaxed);
    T x = nullptr;
    if (t <= b) {
      x = a->get(b);
      if (t == b) {
        // Last element: race with thieves for it.
        if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                          std::memory_order_relaxed))
          x = nullptr;
        bottom_.store(b + 1, std::memory_order_relaxed);
      }
    } else {
      bottom_.store(b + 1, std::memory_order_relaxed);
    }
    return x;
  }

  // Any thread.
  StealResult steal(T& out) {
    int64_t t = top_.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t b = bottom_.load(std::memory_order_acquire);
    if (t < b) {
      // The paper uses memory_order_consume here; acquire is the safe stand-in.
      Array* a = array_.load(std::memory_order_acquire);
      T x = a->get(t);
      if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                        std::memory_order_relaxed))
        return StealResult::Abort;
      out = x;
      return StealResult::Ok;
    }
    return StealResult::Empty;
  }

  // Approximate; for stats/idle checks only.
  int64_t size() const {
    return bottom_.load(std::memory_order_relaxed) - top_.load(std::memory_order_relaxed);
  }

 private:
  Array* grow(Array* old, int64_t t, int64_t b) {
    Array* a = new Array(old->cap * 2);
    for (int64_t i = t; i < b; i++) a->put(i, old->get(i));
    array_.store(a, std::memory_order_release);
    // Thieves may still be reading the old array, so keep it until destruction.
    garbage_.push_back(old);
    return a;
  }

  alignas(64) std::atomic<int64_t> top_{0};
  alignas(64) std::atomic<int64_t> bottom_{0};
  alignas(64) std::atomic<Array*> array_;
  std::vector<Array*> garbage_;  // owner only
};

}  // namespace ws
