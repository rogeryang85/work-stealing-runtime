#include <omp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../src/runtime.hpp"

long fib_serial(int n) {
  if (n < 2) return n;
  return fib_serial(n - 1) + fib_serial(n - 2);
}

long fib_rt(int n, int cutoff) {
  if (n < 2 || n <= cutoff) return fib_serial(n);
  long a, b;
  TaskGroup g;
  g.spawn([&a, n, cutoff] { a = fib_rt(n - 1, cutoff); });
  b = fib_rt(n - 2, cutoff);
  g.wait();
  return a + b;
}

long fib_omp(int n, int cutoff) {
  if (n < 2 || n <= cutoff) return fib_serial(n);
  long a, b;
#pragma omp task shared(a) firstprivate(n, cutoff)
  a = fib_omp(n - 1, cutoff);
  b = fib_omp(n - 2, cutoff);
#pragma omp taskwait
  return a + b;
}

int median_of_3(int a, int b, int c) {
  if (a > b) std::swap(a, b);
  if (b > c) std::swap(b, c);
  if (a > b) std::swap(a, b);
  return b;
}

void partition3(int* a, long n, long* left_size, long* right_size) {
  int pivot = median_of_3(a[0], a[n / 2], a[n - 1]);
  long lt = 0, i = 0, gt = n;
  while (i < gt) {
    if (a[i] < pivot) {
      std::swap(a[i], a[lt]);
      lt++;
      i++;
    } else if (a[i] > pivot) {
      gt--;
      std::swap(a[i], a[gt]);
    } else {
      i++;
    }
  }
  *left_size = lt;
  *right_size = n - gt;
}

void qsort_rt(int* a, long n, long cutoff) {
  if (n <= cutoff) {
    std::sort(a, a + n);
    return;
  }
  long left, right;
  partition3(a, n, &left, &right);
  int* r = a + (n - right);
  TaskGroup g;
  g.spawn([a, left, cutoff] { qsort_rt(a, left, cutoff); });
  qsort_rt(r, right, cutoff);
  g.wait();
}

void qsort_omp(int* a, long n, long cutoff) {
  if (n <= cutoff) {
    std::sort(a, a + n);
    return;
  }
  long left, right;
  partition3(a, n, &left, &right);
  int* r = a + (n - right);
#pragma omp task firstprivate(a, left, cutoff)
  qsort_omp(a, left, cutoff);
  qsort_omp(r, right, cutoff);
#pragma omp taskwait
}

long nqueens_serial(int n, int row, uint32_t cols, uint32_t diag1, uint32_t diag2) {
  if (row == n) return 1;
  long count = 0;
  uint32_t free = ~(cols | diag1 | diag2) & ((1u << n) - 1);
  while (free != 0) {
    uint32_t bit = free & -free;
    free -= bit;
    count += nqueens_serial(n, row + 1, cols | bit, (diag1 | bit) << 1, (diag2 | bit) >> 1);
  }
  return count;
}

long nqueens_rt(int n, int row, uint32_t cols, uint32_t diag1, uint32_t diag2, int cutoff) {
  if (row >= cutoff || row == n) return nqueens_serial(n, row, cols, diag1, diag2);
  long counts[32];
  int k = 0;
  uint32_t free = ~(cols | diag1 | diag2) & ((1u << n) - 1);
  TaskGroup g;
  while (free != 0) {
    uint32_t bit = free & -free;
    free -= bit;
    long* out = &counts[k];
    k++;
    g.spawn([=] {
      *out = nqueens_rt(n, row + 1, cols | bit, (diag1 | bit) << 1, (diag2 | bit) >> 1, cutoff);
    });
  }
  g.wait();
  long total = 0;
  for (int i = 0; i < k; i++) total += counts[i];
  return total;
}

long nqueens_omp(int n, int row, uint32_t cols, uint32_t diag1, uint32_t diag2, int cutoff) {
  if (row >= cutoff || row == n) return nqueens_serial(n, row, cols, diag1, diag2);
  long counts[32];
  int k = 0;
  uint32_t free = ~(cols | diag1 | diag2) & ((1u << n) - 1);
  while (free != 0) {
    uint32_t bit = free & -free;
    free -= bit;
    long* out = &counts[k];
    k++;
#pragma omp task firstprivate(out, bit)
    *out = nqueens_omp(n, row + 1, cols | bit, (diag1 | bit) << 1, (diag2 | bit) >> 1, cutoff);
  }
#pragma omp taskwait
  long total = 0;
  for (int i = 0; i < k; i++) total += counts[i];
  return total;
}

// Binomial UTS (T3 shape, Olivier et al. 2006), with splitmix64 instead of SHA-1.
const int UTS_B0 = 2000;
const int UTS_M = 8;
const double UTS_Q = 0.124875;
int uts_rounds = 64;

uint64_t mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

uint64_t uts_hash(uint64_t id) {
  for (int i = 0; i < uts_rounds; i++) id = mix(id);
  return id;
}

int uts_num_children(uint64_t h, bool is_root) {
  if (is_root) return UTS_B0;
  double u = (h >> 11) / 9007199254740992.0;
  if (u < UTS_Q) return UTS_M;
  return 0;
}

uint64_t uts_child(uint64_t h, int i) {
  return mix(h ^ (0xD1B54A32D192ED03ull * (i + 1)));
}

long uts_serial(uint64_t id, bool is_root) {
  uint64_t h = uts_hash(id);
  int nc = uts_num_children(h, is_root);
  long count = 1;
  for (int i = 0; i < nc; i++) count += uts_serial(uts_child(h, i), false);
  return count;
}

long uts_rt(uint64_t id, bool is_root) {
  uint64_t h = uts_hash(id);
  int nc = uts_num_children(h, is_root);
  if (nc == 0) return 1;
  std::vector<long> counts(nc);
  TaskGroup g;
  for (int i = 0; i < nc - 1; i++) {
    long* out = &counts[i];
    uint64_t child = uts_child(h, i);
    g.spawn([out, child] { *out = uts_rt(child, false); });
  }
  counts[nc - 1] = uts_rt(uts_child(h, nc - 1), false);
  g.wait();
  long total = 1;
  for (int i = 0; i < nc; i++) total += counts[i];
  return total;
}

long uts_omp(uint64_t id, bool is_root) {
  uint64_t h = uts_hash(id);
  int nc = uts_num_children(h, is_root);
  if (nc == 0) return 1;
  std::vector<long> counts(nc);
  for (int i = 0; i < nc - 1; i++) {
    long* out = &counts[i];
    uint64_t child = uts_child(h, i);
#pragma omp task firstprivate(out, child)
    *out = uts_omp(child, false);
  }
  counts[nc - 1] = uts_omp(uts_child(h, nc - 1), false);
#pragma omp taskwait
  long total = 1;
  for (int i = 0; i < nc; i++) total += counts[i];
  return total;
}

const char* bench = "fib";
const char* impl = "ws";
int threads = 1;
int reps = 5;
long n = -1;
long cutoff = -1;
bool check = false;
std::vector<int> data;

long run_serial() {
  if (strcmp(bench, "fib") == 0) return fib_serial(n);
  if (strcmp(bench, "qsort") == 0) {
    std::sort(data.begin(), data.end());
    return 0;
  }
  if (strcmp(bench, "nqueens") == 0) return nqueens_serial(n, 0, 0, 0, 0);
  return uts_serial(n, true);
}

long run_rt() {
  if (strcmp(bench, "fib") == 0) return fib_rt(n, cutoff);
  if (strcmp(bench, "qsort") == 0) {
    qsort_rt(data.data(), n, cutoff);
    return 0;
  }
  if (strcmp(bench, "nqueens") == 0) return nqueens_rt(n, 0, 0, 0, 0, cutoff);
  return uts_rt(n, true);
}

long run_omp() {
  long result = 0;
#pragma omp parallel num_threads(threads)
#pragma omp single
  {
    if (strcmp(bench, "fib") == 0) {
      result = fib_omp(n, cutoff);
    } else if (strcmp(bench, "qsort") == 0) {
      qsort_omp(data.data(), n, cutoff);
    } else if (strcmp(bench, "nqueens") == 0) {
      result = nqueens_omp(n, 0, 0, 0, 0, cutoff);
    } else {
      result = uts_omp(n, true);
    }
  }
  return result;
}

void usage() {
  fprintf(stderr, "usage: bench --bench fib|qsort|nqueens|uts --impl serial|ws|cq|omp\n");
  fprintf(stderr, "             [--threads T] [--reps R] [--n N] [--cutoff C] [--rounds K] [--check]\n");
  exit(2);
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--check") == 0) {
      check = true;
      continue;
    }
    if (i + 1 >= argc) usage();
    if (strcmp(argv[i], "--bench") == 0) bench = argv[i + 1];
    else if (strcmp(argv[i], "--impl") == 0) impl = argv[i + 1];
    else if (strcmp(argv[i], "--threads") == 0) threads = atoi(argv[i + 1]);
    else if (strcmp(argv[i], "--reps") == 0) reps = atoi(argv[i + 1]);
    else if (strcmp(argv[i], "--n") == 0) n = atol(argv[i + 1]);
    else if (strcmp(argv[i], "--cutoff") == 0) cutoff = atol(argv[i + 1]);
    else if (strcmp(argv[i], "--rounds") == 0) uts_rounds = atoi(argv[i + 1]);
    else usage();
    i++;
  }
  if (reps < 1) reps = 1;

  if (strcmp(bench, "fib") == 0) {
    if (n < 0) n = 35;
    if (cutoff < 0) cutoff = 1;
  } else if (strcmp(bench, "qsort") == 0) {
    if (n < 0) n = 20000000;
    if (cutoff < 0) cutoff = 10000;
  } else if (strcmp(bench, "nqueens") == 0) {
    if (n < 0) n = 14;
    if (cutoff < 0) cutoff = 5;
  } else if (strcmp(bench, "uts") == 0) {
    if (n < 0) n = 2;  // root seed; seed 2 gives 2.9M nodes
    cutoff = 0;
  } else {
    usage();
  }

  std::vector<int> original;
  if (strcmp(bench, "qsort") == 0) {
    original.resize(n);
    uint64_t s = 42;
    for (long i = 0; i < n; i++) {
      s = mix(s);
      original[i] = (int)(s >> 33);
    }
  }

  Runtime* rt = nullptr;
  if (strcmp(impl, "ws") == 0) {
    rt = new Runtime(threads, WORK_STEALING);
  } else if (strcmp(impl, "cq") == 0) {
    rt = new Runtime(threads, CENTRAL_QUEUE);
  } else if (strcmp(impl, "serial") != 0 && strcmp(impl, "omp") != 0) {
    usage();
  }

  std::vector<double> times;
  long result = 0;
  Stats stats = {0, 0, 0};
  for (int r = 0; r <= reps; r++) {
    data = original;
    if (rt != nullptr) rt->reset_stats();

    auto start = std::chrono::steady_clock::now();
    if (strcmp(impl, "serial") == 0) {
      result = run_serial();
    } else if (strcmp(impl, "omp") == 0) {
      result = run_omp();
    } else {
      rt->run([&result] { result = run_rt(); });
    }
    auto end = std::chrono::steady_clock::now();

    if (r > 0) times.push_back(std::chrono::duration<double>(end - start).count());
    if (rt != nullptr) stats = rt->total_stats();
    if (strcmp(bench, "qsort") == 0) {
      result = std::is_sorted(data.begin(), data.end()) ? 1 : -1;
    }
  }

  if (check) {
    long expected = 1;
    if (strcmp(bench, "qsort") != 0) expected = run_serial();
    if (result != expected) {
      fprintf(stderr, "CHECK FAILED: %s/%s got %ld expected %ld\n", bench, impl, result, expected);
      return 1;
    }
  }

  std::sort(times.begin(), times.end());
  printf("%s,%s,%d,%ld,%ld,%.6f,%.6f,%ld,%llu,%llu,%llu\n", bench, impl, threads, n, cutoff,
         times[times.size() / 2], times[0], result, (unsigned long long)stats.tasks,
         (unsigned long long)stats.steals, (unsigned long long)stats.steal_attempts);
  delete rt;
  return 0;
}
