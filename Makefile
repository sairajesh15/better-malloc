CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
FPIC ?= -fPIC
SHARED ?= -shared

all: libmem.so test_suite bench_compare real_runner

libmem.so: mem.o
	$(CC) $(SHARED) $(FPIC) -o libmem.so mem.o

mem.o: mem.c mem.h
	$(CC) $(CFLAGS) $(FPIC) -c mem.c

test_suite: test_suite.o libmem.so
	$(CC) -o test_suite test_suite.o -L. -lmem -Wl,-rpath,.

test_suite.o: test_suite.c mem.h
	$(CC) $(CFLAGS) -c test_suite.c

bench_compare: bench_compare.o libmem.so
	$(CC) -o bench_compare bench_compare.o -L. -lmem -Wl,-rpath,.

bench_compare.o: bench_compare.c mem.h
	$(CC) $(CFLAGS) -c bench_compare.c

real_runner: real_runner.o libmem.so
	$(CC) -o real_runner real_runner.o -L. -lmem -Wl,-rpath,.

real_runner.o: real_runner.c mem.h
	$(CC) $(CFLAGS) -c real_runner.c

test: test_suite
	./test_suite

bench: bench_compare
	./bench_compare

serve: all
	python3 server.py

clean:
	rm -f *.o libmem.so test_suite bench_compare real_runner

.PHONY: all clean test bench serve
