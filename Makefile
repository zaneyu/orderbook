CXX ?= c++
STD  := -std=c++20
WARN := -Wall -Wextra -Wpedantic -Werror
INC  := -Iinclude -Itests
TESTS := test_orderbook test_occupancy test_flat_id_map test_alloc_audit

.PHONY: all test bench asan clean
all: test

test:
	@for t in $(TESTS); do \
		echo "== $$t =="; \
		$(CXX) $(STD) $(WARN) -O2 $(INC) tests/$$t.cpp -o $$t && ./$$t || exit 1; \
	done

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
	rm -f ob_bench ob_asan $(TESTS)
