#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "../src/deque.hpp"

Deque<int64_t*>* deque;
std::vector<int64_t> items;
std::atomic<int>* seen;
std::atomic<bool> done;
std::atomic<int64_t> total_stolen;

void thief() {
  int64_t stolen = 0;
  while (!done.load() || deque->size() > 0) {
    int64_t* x = deque->steal();
    if (x != nullptr) {
      seen[*x]++;
      stolen++;
    }
  }
  total_stolen += stolen;
}

int main(int argc, char** argv) {
  int64_t n = 5000000;
  int num_thieves = 7;
  int rounds = 5;
  if (argc > 1) n = atoll(argv[1]);
  if (argc > 2) num_thieves = atoi(argv[2]);
  if (argc > 3) rounds = atoi(argv[3]);

  items.resize(n);
  for (int64_t i = 0; i < n; i++) {
    items[i] = i;
  }

  for (int r = 0; r < rounds; r++) {
    deque = new Deque<int64_t*>(4);
    seen = new std::atomic<int>[n];
    for (int64_t i = 0; i < n; i++) {
      seen[i] = 0;
    }
    done = false;
    total_stolen = 0;

    std::vector<std::thread> thieves;
    for (int k = 0; k < num_thieves; k++) {
      thieves.push_back(std::thread(thief));
    }

    int64_t popped = 0;
    unsigned int seed = 12345 + r;
    int64_t i = 0;
    while (i < n) {
      int burst = 1 + rand_r(&seed) % 64;
      for (int j = 0; j < burst && i < n; j++) {
        deque->push(&items[i]);
        i++;
      }
      int pops = rand_r(&seed) % (burst + 1);
      for (int j = 0; j < pops; j++) {
        int64_t* x = deque->pop();
        if (x != nullptr) {
          seen[*x]++;
          popped++;
        }
      }
    }
    while (true) {
      int64_t* x = deque->pop();
      if (x == nullptr) break;
      seen[*x]++;
      popped++;
    }

    done = true;
    for (int k = 0; k < num_thieves; k++) {
      thieves[k].join();
    }

    int64_t bad = 0;
    for (int64_t j = 0; j < n; j++) {
      if (seen[j] != 1) bad++;
    }
    printf("round %d: popped=%lld stolen=%lld bad=%lld\n", r, (long long)popped,
           (long long)total_stolen.load(), (long long)bad);

    delete deque;
    delete[] seen;
    if (bad != 0 || popped + total_stolen != n) {
      printf("FAIL\n");
      return 1;
    }
  }
  printf("PASS\n");
  return 0;
}
