CXX ?= c++
STD  := -std=c++20
WARN := -Wall -Wextra -Wpedantic
INC  := -Iinclude -Itests

.PHONY: all test bench asan clean
all: test

test:
	$(CXX) $(STD) $(WARN) -O2 $(INC) tests/test_orderbook.cpp -o ob_tests
	./ob_tests

bench:
	$(CXX) $(STD) -O3 -DNDEBUG -march=native -Iinclude bench/bench.cpp -o ob_bench
	./ob_bench

# Address + UB sanitizers over a smaller fuzz sweep (memory safety is exercised
# regardless of op count). Runs in CI; on some sandboxed macOS setups ASan hangs
# at startup, in which case use `-fsanitize=undefined` alone.
asan:
	$(CXX) $(STD) $(WARN) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-DFUZZ_OPS=20000 $(INC) tests/test_orderbook.cpp -o ob_asan
	./ob_asan

clean:
	rm -f ob_tests ob_bench ob_asan
