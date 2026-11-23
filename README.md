# work-stealing-runtime

15-418 final project: a small Cilk-like task runtime in C++. Project page: https://rogeryang85.github.io/work-stealing-runtime/

## Layout

- `src/deque.hpp`: Chase-Lev work-stealing deque (memory orderings from Lê et al., PPoPP 2013)
- `src/runtime.hpp`: the runtime (`ws::Runtime`, `ws::TaskGroup`), with a work-stealing and a central-queue scheduler behind the same API
- `bench/bench.cpp`: fib, quicksort, n-queens, UTS in serial / ours / OpenMP
- `tests/test_deque.cpp`: deque stress test (one owner, many thieves)
- `scripts/sweep.sh`, `scripts/plot.py`: benchmark sweeps and plots

## Usage

```cpp
ws::Runtime rt(8, ws::Sched::WorkStealing);
rt.run([&] {
  ws::TaskGroup g;
  g.spawn([&] { a = fib(n - 1); });
  b = fib(n - 2);
  g.wait();
});
```

## Build and run

Needs GCC (for `-fopenmp`); on macOS use Homebrew `g++-14`.

```sh
make                                   # CXX=g++ on Linux
./build/test_deque                     # should print PASS
./build/bench --bench uts --impl ws --threads 8 --check
scripts/sweep.sh mac && scripts/plot.py mac --pcores 6
```
