CXX = g++-14
CXXFLAGS = -std=c++17 -O3 -march=native -Wall -Wextra -pthread

all: build/bench build/test_deque

build/bench: bench/bench.cpp src/runtime.cpp src/runtime.hpp src/deque.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -fopenmp bench/bench.cpp src/runtime.cpp -o build/bench

build/test_deque: tests/test_deque.cpp src/deque.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) tests/test_deque.cpp -o build/test_deque

clean:
	rm -rf build

.PHONY: all clean
