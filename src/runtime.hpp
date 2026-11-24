#pragma once
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "deque.hpp"

enum SchedulerType { WORK_STEALING, CENTRAL_QUEUE };

struct Task {
  std::function<void()> fn;
  std::atomic<int64_t>* pending;
};

struct Stats {
  uint64_t tasks;
  uint64_t steals;
  uint64_t steal_attempts;
};

class Runtime;

struct alignas(64) Worker {
  int id;
  Runtime* runtime;
  Deque<Task*> deque;
  uint64_t rng;
  Stats stats;
};

class Runtime {
 public:
  Runtime(int num_threads, SchedulerType type);
  ~Runtime();

  void run(std::function<void()> f);

  Stats total_stats();
  void reset_stats();

  void push_task(Worker* w, Task* t);
  Task* find_task(Worker* w);
  void execute_task(Worker* w, Task* t);

 private:
  static void* worker_thread(void* arg);
  static void* root_thread(void* arg);
  void worker_loop(Worker* w);
  int pick_victim(Worker* w);
  pthread_t start_thread(void* (*fn)(void*), void* arg);

  int num_threads;
  SchedulerType type;
  std::vector<Worker*> workers;
  std::vector<pthread_t> threads;

  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<bool> active{false};
  bool stopping = false;
  std::atomic<int> num_parked{0};

  std::function<void()> root_fn;

  std::mutex queue_mutex;
  std::vector<Task*> queue;
  std::atomic<int64_t> queue_size{0};
};

class TaskGroup {
 public:
  TaskGroup();
  ~TaskGroup();
  void spawn(std::function<void()> f);
  void wait();

 private:
  Worker* worker;
  std::atomic<int64_t> pending{0};
};
