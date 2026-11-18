CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
FPIC ?= -fPIC
SHARED ?= -shared

all: libmem.so libmem_test.so test_suite test_suite_interior bench_compare real_runner

# 1. Shipped Production Library (Mem_Reset excluded)
libmem.so: mem.o
	$(CC) $(SHARED) $(FPIC) -o libmem.so mem.o

mem.o: mem.c mem.h
	$(CC) $(CFLAGS) $(FPIC) -c mem.c -o mem.o

# 2. Testing Shared Library (Mem_Reset included for test suite & bridge)
libmem_test.so: mem_test.o
	$(CC) $(SHARED) $(FPIC) -o libmem_test.so mem_test.o

mem_test.o: mem.c mem.h
	$(CC) $(CFLAGS) -DMEM_TESTING $(FPIC) -c mem.c -o mem_test.o

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
	rm -f *.o libmem.so libmem_test.so libmem_interior.so test_suite test_suite_interior bench_compare real_runner

.PHONY: all clean test bench serve
