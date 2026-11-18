#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ARENA_SIZE (64 * 1024 * 1024)
#define NUM_REPS 35
#define WARMUP_REPS 5
#define MEASURE_REPS (NUM_REPS - WARMUP_REPS)

static inline uint64_t get_time_ns(void) {
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static int compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

typedef struct {
    const char *workload_name;
    double median_ns_op;
    double iqr_ns_op;
    double utilization;
    double internal_frag;
    double external_frag;
} bench_stat_result_t;

/* Workload Implementations */

/* 1. Small Allocations (8 to 128 bytes) */
static void run_batch_small(int batch_ops, void **ptrs, double *util, double *int_frag, double *ext_frag) {
    int active_capacity = batch_ops / 2;
    for (int i = 0; i < batch_ops; i++) {
        int slot = i % active_capacity;
        if (ptrs[slot]) {
#ifdef BENCH_SYSTEM
            free(ptrs[slot]);
#else
            Mem_Free(ptrs[slot]);
#endif
            ptrs[slot] = NULL;
        } else {
            int sz = 8 + (i * 17) % 120;
#ifdef BENCH_SYSTEM
            ptrs[slot] = malloc((size_t)sz);
#else
            ptrs[slot] = Mem_Alloc(sz);
#endif
        }
    }
#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0;
#endif
    for (int s = 0; s < active_capacity; s++) {
        if (ptrs[s]) {
#ifdef BENCH_SYSTEM
            free(ptrs[s]);
#else
            Mem_Free(ptrs[s]);
#endif
            ptrs[s] = NULL;
        }
    }
}

/* 2. Mixed Allocations (16 to 2048 bytes) */
static void run_batch_mixed(int batch_ops, void **ptrs, double *util, double *int_frag, double *ext_frag) {
    int active_capacity = batch_ops / 2;
    for (int i = 0; i < batch_ops; i++) {
        int slot = i % active_capacity;
        if (ptrs[slot]) {
#ifdef BENCH_SYSTEM
            free(ptrs[slot]);
#else
            Mem_Free(ptrs[slot]);
#endif
            ptrs[slot] = NULL;
        } else {
            int sz = 16 + (i * 73) % 2032;
#ifdef BENCH_SYSTEM
            ptrs[slot] = malloc((size_t)sz);
#else
            ptrs[slot] = Mem_Alloc(sz);
#endif
        }
    }
#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0;
#endif
    for (int s = 0; s < active_capacity; s++) {
        if (ptrs[s]) {
#ifdef BENCH_SYSTEM
            free(ptrs[s]);
#else
            Mem_Free(ptrs[s]);
#endif
            ptrs[s] = NULL;
        }
    }
}

/* 3. Large Allocations (>4KB up to 64KB) */
static void run_batch_large(int batch_ops, void **ptrs, double *util, double *int_frag, double *ext_frag) {
    int active_capacity = batch_ops / 2;
    for (int i = 0; i < batch_ops; i++) {
        int slot = i % active_capacity;
        if (ptrs[slot]) {
#ifdef BENCH_SYSTEM
            free(ptrs[slot]);
#else
            Mem_Free(ptrs[slot]);
#endif
            ptrs[slot] = NULL;
        } else {
            int sz = 4096 + (i * 127) % 28672;
#ifdef BENCH_SYSTEM
            ptrs[slot] = malloc((size_t)sz);
#else
            ptrs[slot] = Mem_Alloc(sz);
#endif
        }
    }
#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0;
#endif
    for (int s = 0; s < active_capacity; s++) {
        if (ptrs[s]) {
#ifdef BENCH_SYSTEM
            free(ptrs[s]);
#else
            Mem_Free(ptrs[s]);
#endif
            ptrs[s] = NULL;
        }
    }
}

/* 4. LIFO Free (Allocate N, Free in Reverse Order) */
static void run_batch_lifo(int N, void **ptrs, double *util, double *int_frag, double *ext_frag) {
    for (int i = 0; i < N; i++) {
        int sz = 32 + (i * 31) % 256;
#ifdef BENCH_SYSTEM
        ptrs[i] = malloc((size_t)sz);
#else
        ptrs[i] = Mem_Alloc(sz);
#endif
    }
#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0;
#endif
    for (int i = N - 1; i >= 0; i--) {
        if (ptrs[i]) {
#ifdef BENCH_SYSTEM
            free(ptrs[i]);
#else
            Mem_Free(ptrs[i]);
#endif
            ptrs[i] = NULL;
        }
    }
}

/* 5. FIFO Free (Allocate N, Free in FIFO Order) */
static void run_batch_fifo(int N, void **ptrs, double *util, double *int_frag, double *ext_frag) {
    for (int i = 0; i < N; i++) {
        int sz = 32 + (i * 31) % 256;
#ifdef BENCH_SYSTEM
        ptrs[i] = malloc((size_t)sz);
#else
        ptrs[i] = Mem_Alloc(sz);
#endif
    }
#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0;
#endif
    for (int i = 0; i < N; i++) {
        if (ptrs[i]) {
#ifdef BENCH_SYSTEM
            free(ptrs[i]);
#else
            Mem_Free(ptrs[i]);
#endif
            ptrs[i] = NULL;
        }
    }
}

/* 6. Random Free (Allocate N, Free in Permuted Order) */
static void run_batch_random_free(int N, void **ptrs, double *util, double *int_frag, double *ext_frag) {
    int *indices = malloc(N * sizeof(int));
    for (int i = 0; i < N; i++) {
        indices[i] = i;
        int sz = 32 + (i * 31) % 256;
#ifdef BENCH_SYSTEM
        ptrs[i] = malloc((size_t)sz);
#else
        ptrs[i] = Mem_Alloc(sz);
#endif
    }

    /* Pseudo-random Fisher-Yates shuffle */
    uint32_t state = 42;
    for (int i = N - 1; i > 0; i--) {
        state = state * 1664525u + 1013904223u;
        int j = (int)(state % (uint32_t)(i + 1));
        int tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }

#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0;
#endif

    for (int i = 0; i < N; i++) {
        int idx = indices[i];
        if (ptrs[idx]) {
#ifdef BENCH_SYSTEM
            free(ptrs[idx]);
#else
            Mem_Free(ptrs[idx]);
#endif
            ptrs[idx] = NULL;
        }
    }
    free(indices);
}

/* 7. Fragmentation Stress (Allocate N, Free Odds, Free Evens) */
static void run_batch_frag_stress(int N, void **ptrs, double *util, double *int_frag, double *ext_frag) {
    for (int i = 0; i < N; i++) {
        int sz = 64 + (i % 192);
#ifdef BENCH_SYSTEM
        ptrs[i] = malloc((size_t)sz);
#else
        ptrs[i] = Mem_Alloc(sz);
#endif
    }
    for (int i = 1; i < N; i += 2) {
        if (ptrs[i]) {
#ifdef BENCH_SYSTEM
            free(ptrs[i]);
#else
            Mem_Free(ptrs[i]);
#endif
            ptrs[i] = NULL;
        }
    }
#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0;
#endif
    for (int i = 0; i < N; i += 2) {
        if (ptrs[i]) {
#ifdef BENCH_SYSTEM
            free(ptrs[i]);
#else
            Mem_Free(ptrs[i]);
#endif
            ptrs[i] = NULL;
        }
    }
}

/* Generic Workload Benchmark Harness with >= 30 Repetitions */
static bench_stat_result_t benchmark_workload(const char *name, int batch_ops,
    void (*workload_fn)(int, void **, double *, double *, double *)) {
    void **ptrs = calloc(batch_ops, sizeof(void *));
    double samples[MEASURE_REPS];
    double utils[MEASURE_REPS];
    double int_frags[MEASURE_REPS];
    double ext_frags[MEASURE_REPS];

    for (int r = 0; r < NUM_REPS; r++) {
#ifndef BENCH_SYSTEM
        Mem_Reset();
        Mem_Init(ARENA_SIZE);
#endif
        double u = 0, inf = 0, exf = 0;
        uint64_t t0 = get_time_ns();
        workload_fn(batch_ops, ptrs, &u, &inf, &exf);
        uint64_t t1 = get_time_ns();

        if (r >= WARMUP_REPS) {
            int idx = r - WARMUP_REPS;
            samples[idx] = (double)(t1 - t0) / (double)batch_ops;
            utils[idx] = u;
            int_frags[idx] = inf;
            ext_frags[idx] = exf;
        }
    }

    qsort(samples, MEASURE_REPS, sizeof(double), compare_double);
    qsort(utils, MEASURE_REPS, sizeof(double), compare_double);
    qsort(int_frags, MEASURE_REPS, sizeof(double), compare_double);
    qsort(ext_frags, MEASURE_REPS, sizeof(double), compare_double);

    double median = samples[MEASURE_REPS / 2];
    double q1 = samples[MEASURE_REPS / 4];
    double q3 = samples[(3 * MEASURE_REPS) / 4];
    double iqr = q3 - q1;

    free(ptrs);

    bench_stat_result_t res;
    res.workload_name = name;
    res.median_ns_op = median;
    res.iqr_ns_op = iqr;
    res.utilization = utils[MEASURE_REPS / 2];
    res.internal_frag = int_frags[MEASURE_REPS / 2];
    res.external_frag = ext_frags[MEASURE_REPS / 2];
    return res;
}

int main(int argc, char *argv[]) {
    const char *config_name = (argc > 1) ? argv[1] : "default";

    bench_stat_result_t results[7];
    /* Each workload uses batches >= 1000 ops */
    results[0] = benchmark_workload("small_allocs",   2000, run_batch_small);
    results[1] = benchmark_workload("mixed",          2000, run_batch_mixed);
    results[2] = benchmark_workload("large_allocs",   1000, run_batch_large);
    results[3] = benchmark_workload("lifo_free",      2000, run_batch_lifo);
    results[4] = benchmark_workload("fifo_free",      2000, run_batch_fifo);
    results[5] = benchmark_workload("random_free",    2000, run_batch_random_free);
    results[6] = benchmark_workload("frag_stress",    4000, run_batch_frag_stress);

    for (int i = 0; i < 7; i++) {
        printf("RESULT,%s,%s,%.2f,%.2f,%.4f,%.4f,%.4f\n",
               config_name,
               results[i].workload_name,
               results[i].median_ns_op,
               results[i].iqr_ns_op,
               results[i].utilization,
               results[i].internal_frag,
               results[i].external_frag);
    }

    return 0;
}
