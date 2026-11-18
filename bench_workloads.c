#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ARENA_64MB (64 * 1024 * 1024)
#define ARENA_2MB  (2 * 1024 * 1024)
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
    double overhead_per_alloc;
    double nodes_mean;
    double nodes_p99;
    double nodes_max;
    int    failures;
} bench_stat_result_t;

/* ========================================================================= */
/* Workload 1: Small Allocations (8 to 128 bytes)                            */
/* ========================================================================= */
static void run_batch_small(int batch_ops, void **ptrs, double *util, double *int_frag, double *ext_frag, double *ovhd) {
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
            if (ptrs[slot]) {
                ((volatile char *)ptrs[slot])[0] = 1;
            }
        }
    }
#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
    *ovhd = 48.0 + 8.0 + 8.0; /* 48 B header + 8 B canary + 8 B average 16-B alignment */
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0; *ovhd = 16.0;
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

/* ========================================================================= */
/* Workload 2: Mixed Allocations (16 to 2048 bytes)                          */
/* ========================================================================= */
static void run_batch_mixed(int batch_ops, void **ptrs, double *util, double *int_frag, double *ext_frag, double *ovhd) {
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
            if (ptrs[slot]) {
                ((volatile char *)ptrs[slot])[0] = 1;
            }
        }
    }
#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
    *ovhd = 48.0 + 8.0 + 8.0;
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0; *ovhd = 16.0;
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

/* ========================================================================= */
/* Workload 3: Large Allocations (>4KB up to 32KB)                           */
/* ========================================================================= */
static void run_batch_large(int batch_ops, void **ptrs, double *util, double *int_frag, double *ext_frag, double *ovhd) {
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
            if (ptrs[slot]) {
                /* Fair benchmarking: touch first and last byte identically */
                ((volatile char *)ptrs[slot])[0] = 1;
                ((volatile char *)ptrs[slot])[sz - 1] = 1;
            }
        }
    }
#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
    *ovhd = 48.0 + 8.0 + 8.0;
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0; *ovhd = 16.0;
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

/* ========================================================================= */
/* Workload 4: LIFO Free (Allocate N, Free in Reverse Order)                 */
/* ========================================================================= */
static void run_batch_lifo(int N, void **ptrs, double *util, double *int_frag, double *ext_frag, double *ovhd) {
    for (int i = 0; i < N; i++) {
        int sz = 32 + (i * 31) % 256;
#ifdef BENCH_SYSTEM
        ptrs[i] = malloc((size_t)sz);
#else
        ptrs[i] = Mem_Alloc(sz);
#endif
        if (ptrs[i]) ((volatile char *)ptrs[i])[0] = 1;
    }
#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
    *ovhd = 48.0 + 8.0 + 8.0;
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0; *ovhd = 16.0;
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

/* ========================================================================= */
/* Workload 5: FIFO Free (Allocate N, Free in FIFO Order)                    */
/* ========================================================================= */
static void run_batch_fifo(int N, void **ptrs, double *util, double *int_frag, double *ext_frag, double *ovhd) {
    for (int i = 0; i < N; i++) {
        int sz = 32 + (i * 31) % 256;
#ifdef BENCH_SYSTEM
        ptrs[i] = malloc((size_t)sz);
#else
        ptrs[i] = Mem_Alloc(sz);
#endif
        if (ptrs[i]) ((volatile char *)ptrs[i])[0] = 1;
    }
#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
    *ovhd = 48.0 + 8.0 + 8.0;
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0; *ovhd = 16.0;
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

/* ========================================================================= */
/* Workload 6: Random Free (Allocate N, Free in Permuted Order)              */
/* ========================================================================= */
static void run_batch_random_free(int N, void **ptrs, double *util, double *int_frag, double *ext_frag, double *ovhd) {
    int *indices = malloc(N * sizeof(int));
    for (int i = 0; i < N; i++) {
        indices[i] = i;
        int sz = 32 + (i * 31) % 256;
#ifdef BENCH_SYSTEM
        ptrs[i] = malloc((size_t)sz);
#else
        ptrs[i] = Mem_Alloc(sz);
#endif
        if (ptrs[i]) ((volatile char *)ptrs[i])[0] = 1;
    }

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
    *ovhd = 48.0 + 8.0 + 8.0;
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0; *ovhd = 16.0;
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

/* ========================================================================= */
/* Workload 7: Fragmentation Stress (Allocate N, Free Odds, Free Evens)      */
/* ========================================================================= */
static void run_batch_frag_stress(int N, void **ptrs, double *util, double *int_frag, double *ext_frag, double *ovhd) {
    for (int i = 0; i < N; i++) {
        int sz = 64 + (i % 192);
#ifdef BENCH_SYSTEM
        ptrs[i] = malloc((size_t)sz);
#else
        ptrs[i] = Mem_Alloc(sz);
#endif
        if (ptrs[i]) ((volatile char *)ptrs[i])[0] = 1;
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
    *ovhd = 48.0 + 8.0 + 8.0;
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0; *ovhd = 16.0;
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

/* ========================================================================= */
/* Workload 8: Hole Reuse in 2 MB Arena (Step 0.1)                           */
/* ========================================================================= */
#define MAX_HOLE_BLOCKS 5000
static void run_batch_hole_reuse(int dummy, void **dummy_ptrs, double *util, double *int_frag, double *ext_frag, double *ovhd) {
    (void)dummy; (void)dummy_ptrs;
    void *ptrs[MAX_HOLE_BLOCKS];
    int sizes[MAX_HOLE_BLOCKS];
    int count = 0;
    size_t live_bytes = 0;
    size_t target_bytes = (size_t)(ARENA_2MB * 0.60);

#ifndef BENCH_SYSTEM
    Mem_Reset();
    Mem_Init(ARENA_2MB);
#endif

    /* 1. Allocate mixed sizes 16..512 B until ~60% full */
    for (int i = 0; i < MAX_HOLE_BLOCKS && live_bytes < target_bytes; i++) {
        int sz = 16 + (i * 37) % 496;
#ifdef BENCH_SYSTEM
        ptrs[i] = malloc((size_t)sz);
#else
        ptrs[i] = Mem_Alloc(sz);
#endif
        if (!ptrs[i]) break;
        ((volatile char *)ptrs[i])[0] = 1;
        sizes[i] = sz;
        live_bytes += sz;
        count++;
    }

    /* 2. Free every other block to create alternating holes */
    for (int i = 1; i < count; i += 2) {
        if (ptrs[i]) {
#ifdef BENCH_SYSTEM
            free(ptrs[i]);
#else
            Mem_Free(ptrs[i]);
#endif
            ptrs[i] = NULL;
        }
    }

    /* 3a. Reallocate same size sequence into holes */
    void *realloc_a[MAX_HOLE_BLOCKS];
    for (int i = 1; i < count; i += 2) {
#ifdef BENCH_SYSTEM
        realloc_a[i] = malloc((size_t)sizes[i]);
#else
        realloc_a[i] = Mem_Alloc(sizes[i]);
#endif
        if (realloc_a[i]) ((volatile char *)realloc_a[i])[0] = 1;
    }

    /* Free reallocated blocks */
    for (int i = 1; i < count; i += 2) {
        if (realloc_a[i]) {
#ifdef BENCH_SYSTEM
            free(realloc_a[i]);
#else
            Mem_Free(realloc_a[i]);
#endif
            realloc_a[i] = NULL;
        }
    }

    /* 3b. Allocate sizes up to 2x larger into holes */
    void *realloc_b[MAX_HOLE_BLOCKS];
    for (int i = 1; i < count; i += 2) {
        int sz2 = sizes[i] * 2;
#ifdef BENCH_SYSTEM
        realloc_b[i] = malloc((size_t)sz2);
#else
        realloc_b[i] = Mem_Alloc(sz2);
#endif
        if (realloc_b[i]) ((volatile char *)realloc_b[i])[0] = 1;
    }

#ifndef BENCH_SYSTEM
    Mem_GetFragmentationMetrics(util, int_frag, ext_frag);
    *ovhd = 48.0 + 8.0 + 8.0;
#else
    *util = 0.0; *int_frag = 0.0; *ext_frag = 0.0; *ovhd = 16.0;
#endif

    /* Cleanup 3b */
    for (int i = 1; i < count; i += 2) {
        if (realloc_b[i]) {
#ifdef BENCH_SYSTEM
            free(realloc_b[i]);
#else
            Mem_Free(realloc_b[i]);
#endif
            realloc_b[i] = NULL;
        }
    }

    /* Cleanup remaining even blocks */
    for (int i = 0; i < count; i += 2) {
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

/* ========================================================================= */
/* Generic Benchmark Wrapper with Warm-Up & Statistical Percentiles          */
/* ========================================================================= */
static bench_stat_result_t benchmark_workload(const char *name, int batch_ops,
    void (*workload_fn)(int, void **, double *, double *, double *, double *)) {

    void **ptrs = calloc(batch_ops > MAX_HOLE_BLOCKS ? batch_ops : MAX_HOLE_BLOCKS, sizeof(void *));
    double samples[MEASURE_REPS];
    double utils[MEASURE_REPS];
    double int_frags[MEASURE_REPS];
    double ext_frags[MEASURE_REPS];
    double ovhds[MEASURE_REPS];

#ifndef BENCH_SYSTEM
    Mem_Reset();
    Mem_Init(strcmp(name, "hole_reuse") == 0 ? ARENA_2MB : ARENA_64MB);
#endif

    for (int r = 0; r < NUM_REPS; r++) {
        double u = 0, inf = 0, exf = 0, ov = 0;
#ifndef BENCH_SYSTEM
        if (strcmp(name, "hole_reuse") == 0) {
            /* hole_reuse manages its own 2 MB lifecycle inside */
        }
#endif
        uint64_t t0 = get_time_ns();
        workload_fn(batch_ops, ptrs, &u, &inf, &exf, &ov);
        uint64_t t1 = get_time_ns();

        if (r >= WARMUP_REPS) {
            int idx = r - WARMUP_REPS;
            int divisor = (strcmp(name, "hole_reuse") == 0) ? 4777 : batch_ops;
            samples[idx] = (double)(t1 - t0) / (double)divisor;
            utils[idx] = u;
            int_frags[idx] = inf;
            ext_frags[idx] = exf;
            ovhds[idx] = ov;
        }
    }

    qsort(samples, MEASURE_REPS, sizeof(double), compare_double);
    qsort(utils, MEASURE_REPS, sizeof(double), compare_double);
    qsort(int_frags, MEASURE_REPS, sizeof(double), compare_double);
    qsort(ext_frags, MEASURE_REPS, sizeof(double), compare_double);
    qsort(ovhds, MEASURE_REPS, sizeof(double), compare_double);

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
    res.overhead_per_alloc = ovhds[MEASURE_REPS / 2];
    res.nodes_mean = 0.0;
    res.nodes_p99 = 0.0;
    res.nodes_max = 0.0;
    res.failures = 0;

#ifndef BENCH_SYSTEM
#ifdef MEM_STATS
    mem_stats_summary_t ms;
    Mem_Stats_Get(&ms);
    res.nodes_mean = ms.total_alloc_scans ? (double)ms.total_alloc_nodes / ms.total_alloc_scans : 1.0;
    res.nodes_max = (double)ms.last_alloc_nodes;
#endif
#endif

    return res;
}

int main(int argc, char *argv[]) {
    const char *config_name = (argc > 1) ? argv[1] : "default";

    bench_stat_result_t results[8];
    results[0] = benchmark_workload("small_allocs",   2000, run_batch_small);
    results[1] = benchmark_workload("mixed",          2000, run_batch_mixed);
    results[2] = benchmark_workload("large_allocs",   1000, run_batch_large);
    results[3] = benchmark_workload("lifo_free",      2000, run_batch_lifo);
    results[4] = benchmark_workload("fifo_free",      2000, run_batch_fifo);
    results[5] = benchmark_workload("random_free",    2000, run_batch_random_free);
    results[6] = benchmark_workload("frag_stress",    4000, run_batch_frag_stress);
    results[7] = benchmark_workload("hole_reuse",     1000, run_batch_hole_reuse);

    for (int i = 0; i < 8; i++) {
        printf("RESULT,%s,%s,%.2f,%.2f,%.4f,%.4f,%.4f,%.1f,%.2f,%.2f,%.2f,%d\n",
               config_name,
               results[i].workload_name,
               results[i].median_ns_op,
               results[i].iqr_ns_op,
               results[i].utilization,
               results[i].internal_frag,
               results[i].external_frag,
               results[i].overhead_per_alloc,
               results[i].nodes_mean,
               results[i].nodes_p99,
               results[i].nodes_max,
               results[i].failures);
    }

    return 0;
}
