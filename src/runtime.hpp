// A small fork-join task runtime.
//
//   ws::Runtime rt(8, ws::Sched::WorkStealing);
//   rt.run([&] {
//     ws::TaskGroup g;
//     g.spawn([&] { a = work(); });   // may run on another thread
//     b = work();
//     g.wait();                       // "sync"
//   });
//
// Child stealing ("help-first"): spawn pushes the child task and the parent
// keeps going. A thread waiting in TaskGroup::wait() keeps running other
// tasks (its own first, then stolen ones) until its children are done.
//
// Two schedulers share the same API so they can be compared directly:
//   WorkStealing  - one Chase-Lev deque per worker, random victim selection
//   CentralQueue  - one mutex-protected LIFO stack shared by all workers
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>

#include "deque.hpp"

namespace ws {

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

enum class Sched { WorkStealing, CentralQueue };

struct Task {
  std::function<void()> fn;
  std::atomic<int64_t>* pending;  // owning TaskGroup's counter
};

struct Stats {
  uint64_t tasks = 0;           // tasks executed
  uint64_t steals = 0;          // successful steals
  uint64_t steal_attempts = 0;  // includes empty/aborted
};

class Runtime;

struct alignas(64) Worker {
  int id = 0;
  Runtime* rt = nullptr;
  Deque<Task*> dq;
  uint64_t rng = 0;
  Stats st;
};

inline thread_local Worker* tls_worker = nullptr;

class Runtime {
 public:
  Runtime(int nthreads, Sched sched) : n_(nthreads), sched_(sched) {
    workers_.reserve(n_);
    for (int i = 0; i < n_; i++) {
      workers_.emplace_back(new Worker());
      workers_[i]->id = i;
      workers_[i]->rt = this;
      workers_[i]->rng = 0x9E3779B97F4A7C15ull * (i + 1);
    }
    // Worker 0 is a fresh thread made by each run(); start the rest now.
    // pthreads instead of std::thread so we can ask for a big stack: deep task
    // trees (UTS) nest many frames, and macOS gives std::threads only 512 KB.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kStackBytes);
    threads_.resize(n_ > 1 ? n_ - 1 : 0);
    for (int i = 1; i < n_; i++) pthread_create(&threads_[i - 1], &attr, &Runtime::thread_entry, workers_[i].get());
    pthread_attr_destroy(&attr);
  }

  ~Runtime() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) pthread_join(t, nullptr);
  }

  int num_threads() const { return n_; }

  // Run f as worker 0, with the other workers active. Blocks until f returns.
  // f must wait() on every TaskGroup it creates (TaskGroup's destructor does).
  template <class F>
  void run(F&& f) {
    {
      std::lock_guard<std::mutex> lk(m_);
      active_ = true;
    }
    cv_.notify_all();
    // Worker 0 runs f on its own big-stack thread (the main thread's stack is
    // only 8 MB, and help-while-waiting can nest frames deeply).
    struct Root {
      Worker* w;
      F* f;
    } root{workers_[0].get(), &f};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kStackBytes);
    pthread_t th;
    pthread_create(&th, &attr, [](void* arg) -> void* {
      Root* r = static_cast<Root*>(arg);
      tls_worker = r->w;
      (*r->f)();
      return nullptr;
    }, &root);
    pthread_attr_destroy(&attr);
    pthread_join(th, nullptr);
    {
      std::lock_guard<std::mutex> lk(m_);
      active_ = false;
    }
    // Wait for every helper to park so stats are stable between runs.
    while (parked_.load(std::memory_order_acquire) != n_ - 1) std::this_thread::yield();
  }

  Stats stats() const {
    Stats s;
    for (auto& w : workers_) {
      s.tasks += w->st.tasks;
      s.steals += w->st.steals;
      s.steal_attempts += w->st.steal_attempts;
    }
    return s;
  }
  std::vector<Stats> per_worker_stats() const {
    std::vector<Stats> v;
    for (auto& w : workers_) v.push_back(w->st);
    return v;
  }
  void reset_stats() {
    for (auto& w : workers_) w->st = Stats{};
  }

  // ---- used by TaskGroup ----
  void push(Worker* w, Task* t) {
    if (sched_ == Sched::WorkStealing) {
      w->dq.push(t);
    } else {
      std::lock_guard<std::mutex> lk(qm_);
      q_.push_back(t);
      qsize_.store(q_.size(), std::memory_order_relaxed);
    }
  }

  Task* find_task(Worker* w) {
    if (sched_ == Sched::WorkStealing) {
      if (Task* t = w->dq.pop()) return t;
      if (n_ == 1) return nullptr;
      int victim = pick_victim(w);
      w->st.steal_attempts++;
      Task* t = nullptr;
      if (workers_[victim]->dq.steal(t) == Deque<Task*>::StealResult::Ok) {
        w->st.steals++;
        return t;
      }
      return nullptr;
    } else {
      // Test before taking the lock so idle threads don't hammer it.
      if (qsize_.load(std::memory_order_relaxed) == 0) return nullptr;
      std::lock_guard<std::mutex> lk(qm_);
      if (q_.empty()) return nullptr;
      Task* t = q_.back();
      q_.pop_back();
      qsize_.store(q_.size(), std::memory_order_relaxed);
      return t;
    }
  }

  void execute(Worker* w, Task* t) {
    w->st.tasks++;
    t->fn();
    t->pending->fetch_sub(1, std::memory_order_release);
    delete t;
  }

 private:
  int pick_victim(Worker* w) {
    // xorshift64
    uint64_t x = w->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    w->rng = x;
    int v = static_cast<int>(x % (n_ - 1));
    return v >= w->id ? v + 1 : v;  // never pick self
  }

  static void* thread_entry(void* arg) {
    Worker* w = static_cast<Worker*>(arg);
    w->rt->worker_main(w);
    return nullptr;
  }

  void worker_main(Worker* w) {
    tls_worker = w;
    for (;;) {
      {
        std::unique_lock<std::mutex> lk(m_);
        parked_.fetch_add(1, std::memory_order_release);
        cv_.wait(lk, [this] { return active_.load() || stop_; });
        parked_.fetch_sub(1, std::memory_order_relaxed);
        if (stop_) return;
      }
      int fails = 0;
      while (active_flag()) {
        if (Task* t = find_task(w)) {
          execute(w, t);
          fails = 0;
        } else if (++fails < 64) {
          cpu_relax();
        } else {
          std::this_thread::yield();
        }
      }
    }
  }

  bool active_flag() const { return active_.load(std::memory_order_acquire); }

  int n_;
  Sched sched_;
  std::vector<std::unique_ptr<Worker>> workers_;
  static constexpr size_t kStackBytes = 64ull << 20;  // virtual; only touched pages are committed
  std::vector<pthread_t> threads_;

  std::mutex m_;
  std::condition_variable cv_;
  std::atomic<bool> active_{false};  // written under m_ so cv waits don't miss it
  bool stop_ = false;
  std::atomic<int> parked_{0};

  // Central queue
  std::mutex qm_;
  std::vector<Task*> q_;
  std::atomic<size_t> qsize_{0};
};

class TaskGroup {
 public:
  TaskGroup() = default;
  TaskGroup(const TaskGroup&) = delete;
  TaskGroup& operator=(const TaskGroup&) = delete;
  ~TaskGroup() { wait(); }

  template <class F>
  void spawn(F&& f) {
    Worker* w = tls_worker;
    pending_.fetch_add(1, std::memory_order_relaxed);
    w->rt->push(w, new Task{std::function<void()>(std::forward<F>(f)), &pending_});
  }

  void wait() {
    Worker* w = tls_worker;
    while (pending_.load(std::memory_order_acquire) != 0) {
      if (Task* t = w->rt->find_task(w))
        w->rt->execute(w, t);
      else
        cpu_relax();
    }
  }

 private:
  std::atomic<int64_t> pending_{0};
};

}  // namespace ws
