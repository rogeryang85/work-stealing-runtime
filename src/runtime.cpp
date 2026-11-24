#include "runtime.hpp"

#include <thread>

static thread_local Worker* current_worker = nullptr;

// Help-while-waiting can nest many frames, so every thread gets a big stack.
static const size_t STACK_SIZE = 64 * 1024 * 1024;

static void cpu_relax() {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

Runtime::Runtime(int n, SchedulerType t) {
  num_threads = n;
  type = t;
  for (int i = 0; i < num_threads; i++) {
    Worker* w = new Worker();
    w->id = i;
    w->runtime = this;
    w->rng = 0x9E3779B97F4A7C15ull * (i + 1);
    w->stats = Stats{0, 0, 0};
    workers.push_back(w);
  }
  for (int i = 1; i < num_threads; i++) {
    threads.push_back(start_thread(worker_thread, workers[i]));
  }
}

Runtime::~Runtime() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    stopping = true;
  }
  cv.notify_all();
  for (size_t i = 0; i < threads.size(); i++) {
    pthread_join(threads[i], nullptr);
  }
  for (int i = 0; i < num_threads; i++) {
    delete workers[i];
  }
}

pthread_t Runtime::start_thread(void* (*fn)(void*), void* arg) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, STACK_SIZE);
  pthread_t thread;
  pthread_create(&thread, &attr, fn, arg);
  pthread_attr_destroy(&attr);
  return thread;
}

void Runtime::run(std::function<void()> f) {
  root_fn = f;
  {
    std::lock_guard<std::mutex> lock(mutex);
    active = true;
  }
  cv.notify_all();

  pthread_t root = start_thread(root_thread, this);
  pthread_join(root, nullptr);

  {
    std::lock_guard<std::mutex> lock(mutex);
    active = false;
  }
  while (num_parked.load() != num_threads - 1) {
    std::this_thread::yield();
  }
}

void* Runtime::root_thread(void* arg) {
  Runtime* rt = (Runtime*)arg;
  current_worker = rt->workers[0];
  rt->root_fn();
  return nullptr;
}

void* Runtime::worker_thread(void* arg) {
  Worker* w = (Worker*)arg;
  current_worker = w;
  w->runtime->worker_loop(w);
  return nullptr;
}

void Runtime::worker_loop(Worker* w) {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mutex);
      num_parked++;
      while (!active && !stopping) {
        cv.wait(lock);
      }
      num_parked--;
      if (stopping) {
        return;
      }
    }

    int failures = 0;
    while (active.load()) {
      Task* t = find_task(w);
      if (t != nullptr) {
        execute_task(w, t);
        failures = 0;
      } else {
        failures++;
        if (failures < 64) {
          cpu_relax();
        } else {
          std::this_thread::yield();
        }
      }
    }
  }
}

void Runtime::push_task(Worker* w, Task* t) {
  if (type == WORK_STEALING) {
    w->deque.push(t);
  } else {
    std::lock_guard<std::mutex> lock(queue_mutex);
    queue.push_back(t);
    queue_size = queue.size();
  }
}

Task* Runtime::find_task(Worker* w) {
  if (type == WORK_STEALING) {
    Task* t = w->deque.pop();
    if (t != nullptr || num_threads == 1) {
      return t;
    }
    int victim = pick_victim(w);
    w->stats.steal_attempts++;
    t = workers[victim]->deque.steal();
    if (t != nullptr) {
      w->stats.steals++;
    }
    return t;
  }

  if (queue_size.load() == 0) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queue.empty()) {
    return nullptr;
  }
  Task* t = queue.back();
  queue.pop_back();
  queue_size = queue.size();
  return t;
}

void Runtime::execute_task(Worker* w, Task* t) {
  w->stats.tasks++;
  t->fn();
  t->pending->fetch_sub(1, std::memory_order_release);
  delete t;
}

int Runtime::pick_victim(Worker* w) {
  uint64_t x = w->rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  w->rng = x;
  int victim = x % (num_threads - 1);
  if (victim >= w->id) {
    victim++;
  }
  return victim;
}

Stats Runtime::total_stats() {
  Stats s = {0, 0, 0};
  for (int i = 0; i < num_threads; i++) {
    s.tasks += workers[i]->stats.tasks;
    s.steals += workers[i]->stats.steals;
    s.steal_attempts += workers[i]->stats.steal_attempts;
  }
  return s;
}

void Runtime::reset_stats() {
  for (int i = 0; i < num_threads; i++) {
    workers[i]->stats = Stats{0, 0, 0};
  }
}

TaskGroup::TaskGroup() {
  // Look this up once: thread_local reads are a function call with GCC on macOS.
  worker = current_worker;
}

TaskGroup::~TaskGroup() {
  wait();
}

void TaskGroup::spawn(std::function<void()> f) {
  Worker* w = worker;
  pending.fetch_add(1, std::memory_order_relaxed);
  Task* t = new Task();
  t->fn = std::move(f);
  t->pending = &pending;
  w->runtime->push_task(w, t);
}

void TaskGroup::wait() {
  Worker* w = worker;
  while (pending.load(std::memory_order_acquire) != 0) {
    Task* t = w->runtime->find_task(w);
    if (t != nullptr) {
      w->runtime->execute_task(w, t);
    } else {
      cpu_relax();
    }
  }
}
