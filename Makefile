CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
FPIC ?= -fPIC
SHARED ?= -shared

all: libmem.so libmem_test.so test_suite test_suite_interior bench_compare real_runner stress_test

# 1. Shipped Production Library (Mem_Reset excluded)
libmem.so: mem.o
	$(CC) $(SHARED) $(FPIC) -o libmem.so mem.o

mem.o: mem.c mem.h
	$(CC) $(CFLAGS) $(FPIC) -c mem.c -o mem.o

# 2. Testing Shared Library (Mem_Reset included for test suite & bridge)
libmem_test.so: mem_test.o
	$(CC) $(SHARED) $(FPIC) -o libmem_test.so mem_test.o

mem_test.o: mem.c mem.h
	$(CC) $(CFLAGS) -DMEM_TESTING -DMEM_STATS $(FPIC) -c mem.c -o mem_test.o

# 3. Testing Shared Library with MEM_ALLOW_INTERIOR=1
libmem_interior.so: mem_interior.o
	$(CC) $(SHARED) $(FPIC) -o libmem_interior.so mem_interior.o

mem_interior.o: mem.c mem.h
	$(CC) $(CFLAGS) -DMEM_TESTING -DMEM_ALLOW_INTERIOR=1 $(FPIC) -c mem.c -o mem_interior.o

# 4. Standard Test Suite (MEM_ALLOW_INTERIOR=0, Strict O(1))
test_suite: test_suite.o libmem_test.so
	$(CC) -o test_suite test_suite.o -L. -lmem_test -Wl,-rpath,.

test_suite.o: test_suite.c mem.h
	$(CC) $(CFLAGS) -DMEM_TESTING -c test_suite.c -o test_suite.o

# 5. Interior Pointer Test Suite (MEM_ALLOW_INTERIOR=1)
test_suite_interior: test_suite_interior.o libmem_interior.so
	$(CC) -o test_suite_interior test_suite_interior.o -L. -lmem_interior -Wl,-rpath,.

test_suite_interior.o: test_suite.c mem.h
	$(CC) $(CFLAGS) -DMEM_TESTING -DMEM_ALLOW_INTERIOR=1 -c test_suite.c -o test_suite_interior.o

# 6. Benchmark Suite
bench_compare: bench_compare.o libmem_test.so
	$(CC) -o bench_compare bench_compare.o -L. -lmem_test -Wl,-rpath,.

bench_compare.o: bench_compare.c mem.h
	$(CC) $(CFLAGS) -DMEM_TESTING -c bench_compare.c -o bench_compare.o

# 7. Real Hardware Runner
real_runner: real_runner.o libmem_test.so
	$(CC) -o real_runner real_runner.o -L. -lmem_test -Wl,-rpath,.

real_runner.o: real_runner.c mem.h
	$(CC) $(CFLAGS) -DMEM_TESTING -c real_runner.c -o real_runner.o

# 8. Randomized Stress Test
stress_test: stress_test.o libmem_test.so
	$(CC) -o stress_test stress_test.o -L. -lmem_test -Wl,-rpath,.

stress_test.o: stress_test.c mem.h
	$(CC) $(CFLAGS) -DMEM_TESTING -c stress_test.c -o stress_test.o

stress: stress_test
	./stress_test 1337

# 9. Debug Stress Test with Mem_Check after every op
stress_test_debug: stress_test_debug.o libmem_test.so
	$(CC) -o stress_test_debug stress_test_debug.o -L. -lmem_test -Wl,-rpath,.

stress_test_debug.o: stress_test.c mem.h
	$(CC) $(CFLAGS) -DMEM_TESTING -DSTRESS_DEBUG -c stress_test.c -o stress_test_debug.o

stress_debug: stress_test_debug
	./stress_test_debug 1337 5000

# 10. Sanitized Builds: UBSan on allocator and harness, ASan on harness only
mem_ubsan.o: mem.c mem.h
	$(CC) $(CFLAGS) -DMEM_TESTING -fsanitize=undefined $(FPIC) -c mem.c -o mem_ubsan.o

libmem_ubsan.so: mem_ubsan.o
	$(CC) $(SHARED) $(FPIC) -fsanitize=undefined -o libmem_ubsan.so mem_ubsan.o

test_suite_sanitize.o: test_suite.c mem.h
	$(CC) $(CFLAGS) -DMEM_TESTING -fsanitize=undefined,address -c test_suite.c -o test_suite_sanitize.o

test_suite_sanitize: test_suite_sanitize.o libmem_ubsan.so
	$(CC) -fsanitize=undefined,address -o test_suite_sanitize test_suite_sanitize.o -L. -lmem_ubsan -Wl,-rpath,.

sanitize: test_suite_sanitize
	@echo "\n>>> RUNNING SANITIZED TEST SUITE (UBSan on allocator & harness, ASan on harness) <<<"
	./test_suite_sanitize

# 11. Diagnostic Run for Phase 3a
diagnose_fragmentation: diagnose_fragmentation.o libmem_test.so
	$(CC) -o diagnose_fragmentation diagnose_fragmentation.o -L. -lmem_test -Wl,-rpath,.

diagnose_fragmentation.o: diagnose_fragmentation.c mem.h
	$(CC) $(CFLAGS) -DMEM_TESTING -DMEM_STATS -c diagnose_fragmentation.c -o diagnose_fragmentation.o

diagnose: diagnose_fragmentation
	./diagnose_fragmentation 50000

# 12. Benchmark Workloads Matrix (Phase 3b)
matrix:
	python3 run_benchmark_matrix.py

test: test_suite test_suite_interior
	@echo "\n>>> RUNNING TEST SUITE (MEM_ALLOW_INTERIOR=0, Strict O(1) Path) <<<"
	./test_suite
	@echo "\n>>> RUNNING TEST SUITE (MEM_ALLOW_INTERIOR=1, Interior Pointer Fallback Path) <<<"
	./test_suite_interior

bench: bench_compare
	./bench_compare

serve: all
	python3 server.py

clean:
	rm -f *.o libmem.so libmem_test.so libmem_interior.so libmem_ubsan.so test_suite test_suite_interior test_suite_sanitize bench_compare real_runner stress_test stress_test_debug diagnose_fragmentation

.PHONY: all clean test bench serve stress stress_debug sanitize diagnose matrix
