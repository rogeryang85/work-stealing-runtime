// Stress test: one owner pushes/pops, several thieves steal. Every item must be
// taken exactly once. Run on ARM (weak memory) as well as x86.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "../src/deque.hpp"

int main(int argc, char** argv) {
  const int64_t N = argc > 1 ? atoll(argv[1]) : 5'000'000;
  const int nthieves = argc > 2 ? atoi(argv[2]) : 7;
  const int rounds = argc > 3 ? atoi(argv[3]) : 5;

  for (int r = 0; r < rounds; r++) {
    // Small initial capacity so grow() gets exercised under contention.
    ws::Deque<int64_t*> dq(4);
    std::vector<int64_t> items(N);
    std::vector<std::atomic<uint8_t>> seen(N);
    for (int64_t i = 0; i < N; i++) items[i] = i;
    std::atomic<bool> done{false};
    std::atomic<int64_t> stolen{0}, popped{0};

    std::vector<std::thread> thieves;
    for (int k = 0; k < nthieves; k++) {
      thieves.emplace_back([&] {
        int64_t local = 0;
        while (!done.load(std::memory_order_acquire) || dq.size() > 0) {
          int64_t* x = nullptr;
          if (dq.steal(x) == ws::Deque<int64_t*>::StealResult::Ok) {
            seen[*x].fetch_add(1, std::memory_order_relaxed);
            local++;
          }
        }
        stolen += local;
      });
    }

    // Owner: push in bursts, pop some back (like spawn/sync).
    int64_t local = 0;
    uint64_t rng = 12345 + r;
    for (int64_t i = 0; i < N;) {
      rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
      int burst = 1 + rng % 64;
      for (int j = 0; j < burst && i < N; j++, i++) dq.push(&items[i]);
      int pops = rng % (burst + 1);
      for (int j = 0; j < pops; j++) {
        if (int64_t* x = dq.pop()) {
          seen[*x].fetch_add(1, std::memory_order_relaxed);
          local++;
        }
      }
    }
    while (int64_t* x = dq.pop()) {
      seen[*x].fetch_add(1, std::memory_order_relaxed);
      local++;
    }
    popped = local;
    done.store(true, std::memory_order_release);
    for (auto& t : thieves) t.join();

    int64_t bad = 0;
    for (int64_t i = 0; i < N; i++)
      if (seen[i].load() != 1) bad++;
    printf("round %d: popped=%lld stolen=%lld total=%lld bad=%lld\n", r, (long long)popped.load(),
           (long long)stolen.load(), (long long)(popped + stolen), (long long)bad);
    if (bad || popped + stolen != N) {
      printf("FAIL\n");
      return 1;
    }
  }
  printf("PASS\n");
  return 0;
}
