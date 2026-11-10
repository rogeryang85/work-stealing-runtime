---
title: A Small Work-Stealing Task Runtime
---

**Roger Yang (hryang)** · 15-418, Fall 2026

A small Cilk-like task runtime in C++ with spawn/sync and per-thread work-stealing deques, benchmarked on the GHC machines against a shared-queue runtime and OpenMP tasks.

## Reports

- [Proposal](proposal/proposal.pdf) (Nov 9)
- Milestone report (Nov 23): coming soon
- Final report (Dec 10): coming soon

## Schedule

| Week | Tasks | Status |
|---|---|---|
| Nov 9–15 | Thread pool, deque, stress tests on ARM and x86, spawn/sync, fib working | |
| Nov 16–22 | Quicksort, n-queens, UTS; serial and OpenMP versions; central queue; first speedup graphs | |
| Nov 23 | **Milestone due** | |
| Nov 23–29 | Task pools, per-thread stats, more steal policies | |
| Nov 30–Dec 6 | Policy and granularity experiments; stretch goals | |
| Dec 7–10 | Final runs, timeline figure, report | |
| Dec 11 | **Poster session** | |
