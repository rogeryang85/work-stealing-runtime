// Benchmark driver.
//
//   bench --bench fib|qsort|nqueens|uts --impl serial|ws|cq|omp
//         [--threads T] [--reps R] [--n N] [--cutoff C] [--check]
//
// Prints one CSV line:
//   bench,impl,threads,n,cutoff,median_s,min_s,result,tasks,steals,steal_attempts
#include <omp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/runtime.hpp"

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- fib
// Tasks spawn all the way down unless n <= cutoff (then serial).

static long fib_serial(int n) { return n < 2 ? n : fib_serial(n - 1) + fib_serial(n - 2); }

static long fib_rt(int n, int cutoff) {
  if (n <= cutoff || n < 2) return fib_serial(n);
  long a, b;
  ws::TaskGroup g;
  g.spawn([&a, n, cutoff] { a = fib_rt(n - 1, cutoff); });
  b = fib_rt(n - 2, cutoff);
  g.wait();
  return a + b;
}

static long fib_omp(int n, int cutoff) {
  if (n <= cutoff || n < 2) return fib_serial(n);
  long a, b;
#pragma omp task shared(a) firstprivate(n, cutoff)
  a = fib_omp(n - 1, cutoff);
  b = fib_omp(n - 2, cutoff);
#pragma omp taskwait
  return a + b;
}

// ---------------------------------------------------------------- quicksort
// Serial partition, recurse in parallel, std::sort below cutoff elements.

static int median3(int a, int b, int c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

template <class Spawn>
static void qs_step(int* a, long n, Spawn&& recurse) {
  int p = median3(a[0], a[n / 2], a[n - 1]);
  int* lt = std::partition(a, a + n, [p](int x) { return x < p; });
  int* gt = std::partition(lt, a + n, [p](int x) { return !(p < x); });  // equal block
  recurse(a, lt - a, gt, (a + n) - gt);
}

static void qs_rt(int* a, long n, long cutoff) {
  if (n <= cutoff) { std::sort(a, a + n); return; }
  qs_step(a, n, [cutoff](int* l, long ln, int* r, long rn) {
    ws::TaskGroup g;
    g.spawn([=] { qs_rt(l, ln, cutoff); });
    qs_rt(r, rn, cutoff);
    g.wait();
  });
}

static void qs_omp(int* a, long n, long cutoff) {
  if (n <= cutoff) { std::sort(a, a + n); return; }
  qs_step(a, n, [cutoff](int* l, long ln, int* r, long rn) {
#pragma omp task firstprivate(l, ln, cutoff)
    qs_omp(l, ln, cutoff);
    qs_omp(r, rn, cutoff);
#pragma omp taskwait
  });
}

// ---------------------------------------------------------------- n-queens
// Bitmask search; spawn a task per legal placement while row < cutoff.

static long nq_serial(int n, int row, uint32_t cols, uint32_t d1, uint32_t d2) {
  if (row == n) return 1;
  long count = 0;
  uint32_t avail = ~(cols | d1 | d2) & ((1u << n) - 1);
  while (avail) {
    uint32_t bit = avail & -avail;
    avail -= bit;
    count += nq_serial(n, row + 1, cols | bit, (d1 | bit) << 1, (d2 | bit) >> 1);
  }
  return count;
}

static long nq_rt(int n, int row, uint32_t cols, uint32_t d1, uint32_t d2, int cutoff) {
  if (row >= cutoff) return nq_serial(n, row, cols, d1, d2);
  if (row == n) return 1;
  long cnt[32] = {0};
  int k = 0;
  uint32_t avail = ~(cols | d1 | d2) & ((1u << n) - 1);
  {
    ws::TaskGroup g;
    while (avail) {
      uint32_t bit = avail & -avail;
      avail -= bit;
      long* out = &cnt[k++];
      g.spawn([=] { *out = nq_rt(n, row + 1, cols | bit, (d1 | bit) << 1, (d2 | bit) >> 1, cutoff); });
    }
    g.wait();
  }
  long s = 0;
  for (int i = 0; i < k; i++) s += cnt[i];
  return s;
}

static long nq_omp(int n, int row, uint32_t cols, uint32_t d1, uint32_t d2, int cutoff) {
  if (row >= cutoff) return nq_serial(n, row, cols, d1, d2);
  if (row == n) return 1;
  long cnt[32] = {0};
  int k = 0;
  uint32_t avail = ~(cols | d1 | d2) & ((1u << n) - 1);
  while (avail) {
    uint32_t bit = avail & -avail;
    avail -= bit;
    long* out = &cnt[k++];
#pragma omp task firstprivate(out, bit)
    *out = nq_omp(n, row + 1, cols | bit, (d1 | bit) << 1, (d2 | bit) >> 1, cutoff);
  }
#pragma omp taskwait
  long s = 0;
  for (int i = 0; i < k; i++) s += cnt[i];
  return s;
}

// ---------------------------------------------------------------- UTS
// Binomial Unbalanced Tree Search (T3 shape from Olivier et al. 2006):
// root has B0 children; every other node has M children with probability Q,
// otherwise none. QM = 0.999; tree size depends a lot on the root seed (seed 2 -> 2.9M nodes). The real UTS
// uses SHA-1 for its splittable RNG; we use iterated splitmix64 instead, with
// kUtsRounds rounds standing in for the per-node hashing work.

static constexpr int kUtsB0 = 2000;
static constexpr int kUtsM = 8;
static constexpr double kUtsQ = 0.124875;
static int kUtsRounds = 64;

static inline uint64_t mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}
static inline uint64_t uts_work(uint64_t id) {
  for (int i = 0; i < kUtsRounds; i++) id = mix(id);
  return id;
}
static inline int uts_children(uint64_t h, bool root) {
  if (root) return kUtsB0;
  double u = (h >> 11) * (1.0 / 9007199254740992.0);
  return u < kUtsQ ? kUtsM : 0;
}
static inline uint64_t uts_child_id(uint64_t h, int i) { return mix(h ^ (0xD1B54A32D192ED03ull * (i + 1))); }

static long uts_serial(uint64_t id, bool root) {
  uint64_t h = uts_work(id);
  int nc = uts_children(h, root);
  long count = 1;
  for (int i = 0; i < nc; i++) count += uts_serial(uts_child_id(h, i), false);
  return count;
}

static long uts_rt(uint64_t id, bool root) {
  uint64_t h = uts_work(id);
  int nc = uts_children(h, root);
  if (nc == 0) return 1;
  std::vector<long> big;
  long small[kUtsM];
  long* cnt = small;
  if (nc > kUtsM) { big.resize(nc); cnt = big.data(); }
  {
    ws::TaskGroup g;
    for (int i = 0; i < nc - 1; i++) {
      long* out = &cnt[i];
      uint64_t cid = uts_child_id(h, i);
      g.spawn([out, cid] { *out = uts_rt(cid, false); });
    }
    cnt[nc - 1] = uts_rt(uts_child_id(h, nc - 1), false);
    g.wait();
  }
  long count = 1;
  for (int i = 0; i < nc; i++) count += cnt[i];
  return count;
}

static long uts_omp(uint64_t id, bool root) {
  uint64_t h = uts_work(id);
  int nc = uts_children(h, root);
  if (nc == 0) return 1;
  std::vector<long> big;
  long small[kUtsM];
  long* cnt = small;
  if (nc > kUtsM) { big.resize(nc); cnt = big.data(); }
  for (int i = 0; i < nc - 1; i++) {
    long* out = &cnt[i];
    uint64_t cid = uts_child_id(h, i);
#pragma omp task firstprivate(out, cid)
    *out = uts_omp(cid, false);
  }
  cnt[nc - 1] = uts_omp(uts_child_id(h, nc - 1), false);
#pragma omp taskwait
  long count = 1;
  for (int i = 0; i < nc; i++) count += cnt[i];
  return count;
}

// ---------------------------------------------------------------- driver

struct Opts {
  std::string bench = "fib", impl = "ws";
  int threads = 1, reps = 5;
  long n = -1, cutoff = -1;
  bool check = false;
};

static void usage() {
  fprintf(stderr,
          "usage: bench --bench fib|qsort|nqueens|uts --impl serial|ws|cq|omp\n"
          "             [--threads T] [--reps R] [--n N] [--cutoff C] [--rounds K] [--check]\n");
  exit(2);
}

int main(int argc, char** argv) {
  Opts o;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { if (i + 1 >= argc) usage(); return argv[++i]; };
    if (a == "--bench") o.bench = next();
    else if (a == "--impl") o.impl = next();
    else if (a == "--threads") o.threads = atoi(next());
    else if (a == "--reps") o.reps = atoi(next());
    else if (a == "--n") o.n = atol(next());
    else if (a == "--cutoff") o.cutoff = atol(next());
    else if (a == "--rounds") kUtsRounds = atoi(next());
    else if (a == "--check") o.check = true;
    else usage();
  }

  if (o.reps < 1) o.reps = 1;

  // Defaults chosen so serial runs take very roughly 0.3-1.5 s.
  if (o.bench == "fib") { if (o.n < 0) o.n = 35; if (o.cutoff < 0) o.cutoff = 1; }
  else if (o.bench == "qsort") { if (o.n < 0) o.n = 20'000'000; if (o.cutoff < 0) o.cutoff = 10'000; }
  else if (o.bench == "nqueens") { if (o.n < 0) o.n = 14; if (o.cutoff < 0) o.cutoff = 5; }
  else if (o.bench == "uts") { if (o.n < 0) o.n = 2; o.cutoff = 0; }  // n = root seed; seed 2 -> 2.9M nodes
  else usage();

  // Input for quicksort (regenerated before every rep, not timed).
  std::vector<int> orig, data;
  if (o.bench == "qsort") {
    orig.resize(o.n);
    uint64_t s = 42;
    for (auto& x : orig) { s = mix(s); x = static_cast<int>(s >> 33); }
  }

  auto run_serial = [&]() -> long {
    if (o.bench == "fib") return fib_serial(o.n);
    if (o.bench == "qsort") { std::sort(data.begin(), data.end()); return 0; }
    if (o.bench == "nqueens") return nq_serial(o.n, 0, 0, 0, 0);
    return uts_serial(o.n, true);
  };
  auto run_rt = [&]() -> long {
    if (o.bench == "fib") return fib_rt(o.n, o.cutoff);
    if (o.bench == "qsort") { qs_rt(data.data(), o.n, o.cutoff); return 0; }
    if (o.bench == "nqueens") return nq_rt(o.n, 0, 0, 0, 0, o.cutoff);
    return uts_rt(o.n, true);
  };
  auto run_omp = [&]() -> long {
    long r = 0;
#pragma omp parallel num_threads(o.threads)
#pragma omp single
    {
      if (o.bench == "fib") r = fib_omp(o.n, o.cutoff);
      else if (o.bench == "qsort") qs_omp(data.data(), o.n, o.cutoff);
      else if (o.bench == "nqueens") r = nq_omp(o.n, 0, 0, 0, 0, o.cutoff);
      else r = uts_omp(o.n, true);
    }
    return r;
  };

  std::unique_ptr<ws::Runtime> rt;
  if (o.impl == "ws") rt.reset(new ws::Runtime(o.threads, ws::Sched::WorkStealing));
  else if (o.impl == "cq") rt.reset(new ws::Runtime(o.threads, ws::Sched::CentralQueue));
  else if (o.impl != "serial" && o.impl != "omp") usage();

  std::vector<double> times;
  long result = 0;
  ws::Stats st;
  for (int r = 0; r <= o.reps; r++) {  // rep 0 is warmup
    if (o.bench == "qsort") data = orig;
    if (rt) rt->reset_stats();
    auto t0 = Clock::now();
    if (o.impl == "serial") result = run_serial();
    else if (o.impl == "omp") result = run_omp();
    else rt->run([&] { result = run_rt(); });
    double sec = std::chrono::duration<double>(Clock::now() - t0).count();
    if (r > 0) times.push_back(sec);
    if (rt) st = rt->stats();
    if (o.bench == "qsort") result = std::is_sorted(data.begin(), data.end()) ? 1 : -1;
  }

  if (o.check) {
    long expect;
    if (o.bench == "qsort") expect = 1;
    else { data = orig; expect = run_serial(); }
    if (result != expect) {
      fprintf(stderr, "CHECK FAILED: %s/%s got %ld expected %ld\n", o.bench.c_str(), o.impl.c_str(), result, expect);
      return 1;
    }
  }

  std::sort(times.begin(), times.end());
  double med = times[times.size() / 2], mn = times[0];
  printf("%s,%s,%d,%ld,%ld,%.6f,%.6f,%ld,%llu,%llu,%llu\n", o.bench.c_str(), o.impl.c_str(), o.threads, o.n,
         o.cutoff, med, mn, result, (unsigned long long)st.tasks, (unsigned long long)st.steals,
         (unsigned long long)st.steal_attempts);
  return 0;
}
