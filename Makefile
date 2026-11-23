CXX      = g++-14
CXXFLAGS ?= -std=c++17 -O3 -march=native -Wall -Wextra -pthread

all: build/bench build/test_deque

build/bench: bench/bench.cpp src/runtime.hpp src/deque.hpp | build
	$(CXX) $(CXXFLAGS) -fopenmp $< -o $@

build/test_deque: tests/test_deque.cpp src/deque.hpp | build
	$(CXX) $(CXXFLAGS) $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean
