---
title: A Small Work-Stealing Task Runtime
---

**Roger Yang (hryang)** · 15-418, Fall 2026

A small Cilk-like task runtime in C++ with spawn/sync and per-thread work-stealing deques, benchmarked against a shared-queue runtime and OpenMP tasks.

## Reports

- [Proposal](proposal/proposal.pdf) (Nov 9)
- [Milestone report](milestone/milestone.pdf) (Nov 23)
- Final report (Dec 10): coming soon

## Progress (Nov 23)

The runtime works: Chase-Lev deques, spawn/sync, random stealing, plus a central-queue scheduler for comparison. All four benchmarks (fib, quicksort, n-queens, UTS) are done in serial, my runtime and OpenMP versions. Preliminary numbers are from my laptop (M3 Pro); GHC runs are next.

![Speedup on the M3 Pro](milestone/mac_speedup.png)

## Schedule

| When | Task | Status |
|---|---|---|
| Nov 9–15 | Thread pool, deque, stress tests on ARM, spawn/sync, fib working | done |
| Nov 16–22 | Quicksort, n-queens, UTS; serial and OpenMP versions; central queue; first speedup graphs | done |
| Nov 23 | **Milestone** | done |
| Nov 23–25 | Sweep on GHC; add libomp; bigger inputs for short benchmarks | |
| Nov 26–29 | Per-thread task pools; overhead breakdown for fib | |
| Nov 30–Dec 2 | Round-robin and last-victim steal policies | |
| Dec 3–6 | Idle-time stats, UTS timeline plot; idle policy study if time | |
| Dec 7–8 | Final GHC runs; lazy binary splitting if ahead | |
| Dec 9–10 | Final report | |
| Dec 11 | **Poster session** | |
