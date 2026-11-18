#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define ARENA_SIZE (64 * 1024 * 1024)

static int compare_u64(const void *a, const void *b) {
    uint64_t u1 = *(const uint64_t *)a;
    uint64_t u2 = *(const uint64_t *)b;
    if (u1 < u2) return -1;
    if (u1 > u2) return 1;
    return 0;
}

int main(int argc, char *argv[]) {
    int total_ops = 50000;
    if (argc > 1) {
        total_ops = atoi(argv[1]);
        if (total_ops < 100) total_ops = 50000;
    }

    int N = total_ops / 2;
    printf("================================================================================\n");
    printf("       PROJECT 23: PHASE 3a DIAGNOSTIC RUN - FRAGMENTATION WORKLOAD             \n");
    printf("================================================================================\n");
    printf("Parameters:\n");
    printf("  - Total Operations : %d (N = %d allocations + %d frees)\n", total_ops, N, N);
    printf("  - Arena Size       : %d MB (%d bytes)\n", ARENA_SIZE / (1024 * 1024), ARENA_SIZE);
    printf("  - Allocator Policy : Segregated Bins (First-Fit Search, Address-Ordered Insertion)\n");
    printf("--------------------------------------------------------------------------------\n\n");

    Mem_Reset();
    if (Mem_Init(ARENA_SIZE) != 0) {
        fprintf(stderr, "Mem_Init failed!\n");
        return 1;
    }

    void **ptrs = calloc(N, sizeof(void *));
    uint64_t *alloc_samples = malloc(N * sizeof(uint64_t));
    uint64_t *insert_samples = malloc(N * 2 * sizeof(uint64_t));
    size_t insert_sample_count = 0;

    Mem_Stats_Reset();

    /* 1. Allocate N blocks of alternating sizes */
    for (int i = 0; i < N; i++) {
        int sz = 64 + (i % 192);
        ptrs[i] = Mem_Alloc(sz);
        if (!ptrs[i]) {
            fprintf(stderr, "Alloc failed at index %d!\n", i);
            return 1;
        }
        alloc_samples[i] = Mem_Stats_GetLastAllocNodes();
    }

    /* 2. Free odd blocks: creates alternating free holes */
    for (int i = 1; i < N; i += 2) {
        if (ptrs[i]) {
            Mem_Free(ptrs[i]);
            ptrs[i] = NULL;
            insert_samples[insert_sample_count++] = Mem_Stats_GetLastInsertNodes();
        }
    }

    /* 3. Free even blocks: triggers bi-directional coalescing */
    for (int i = 0; i < N; i += 2) {
        if (ptrs[i]) {
            Mem_Free(ptrs[i]);
            ptrs[i] = NULL;
            insert_samples[insert_sample_count++] = Mem_Stats_GetLastInsertNodes();
        }
    }

    mem_stats_summary_t stats;
    Mem_Stats_Get(&stats);

    /* Sort samples for percentiles */
    qsort(alloc_samples, N, sizeof(uint64_t), compare_u64);
    qsort(insert_samples, insert_sample_count, sizeof(uint64_t), compare_u64);

    double alloc_mean = (double)stats.total_alloc_nodes / (stats.total_alloc_scans ? stats.total_alloc_scans : 1);
    uint64_t alloc_p50 = alloc_samples[(size_t)(N * 0.50)];
    uint64_t alloc_p90 = alloc_samples[(size_t)(N * 0.90)];
    uint64_t alloc_p99 = alloc_samples[(size_t)(N * 0.99)];
    uint64_t alloc_max = alloc_samples[N - 1];

    double insert_mean = (double)stats.total_insert_nodes / (stats.total_insert_ops ? stats.total_insert_ops : 1);
    uint64_t insert_p50 = insert_samples[(size_t)(insert_sample_count * 0.50)];
    uint64_t insert_p90 = insert_samples[(size_t)(insert_sample_count * 0.90)];
    uint64_t insert_p99 = insert_samples[(size_t)(insert_sample_count * 0.99)];
    uint64_t insert_max = insert_samples[insert_sample_count - 1];

    printf("================================================================================\n");
    printf("                  PHASE 3a EMPIRICAL TELEMETRY RESULTS                          \n");
    printf("================================================================================\n");
    printf("1. Nodes Visited per Mem_Alloc Scan (Total Scans: %llu, Total Nodes: %llu):\n",
           (unsigned long long)stats.total_alloc_scans, (unsigned long long)stats.total_alloc_nodes);
    printf("   - Mean : %8.2f nodes\n", alloc_mean);
    printf("   - p50  : %8llu nodes\n", (unsigned long long)alloc_p50);
    printf("   - p90  : %8llu nodes\n", (unsigned long long)alloc_p90);
    printf("   - p99  : %8llu nodes\n", (unsigned long long)alloc_p99);
    printf("   - Max  : %8llu nodes\n", (unsigned long long)alloc_max);
    printf("\n");
    printf("2. Nodes Visited per insert_into_bin Traversal (Total Inserts: %llu, Total Nodes: %llu):\n",
           (unsigned long long)stats.total_insert_ops, (unsigned long long)stats.total_insert_nodes);
    printf("   - Mean : %8.2f nodes\n", insert_mean);
    printf("   - p50  : %8llu nodes\n", (unsigned long long)insert_p50);
    printf("   - p90  : %8llu nodes\n", (unsigned long long)insert_p90);
    printf("   - p99  : %8llu nodes\n", (unsigned long long)insert_p99);
    printf("   - Max  : %8llu nodes\n", (unsigned long long)insert_max);
    printf("\n");
    printf("3. Lifecycle Mutation Counters:\n");
    printf("   - Total Block Splits     : %llu\n", (unsigned long long)stats.total_splits);
    printf("   - Total Block Coalesces  : %llu\n", (unsigned long long)stats.total_coalesces);
    printf("================================================================================\n\n");

    free(ptrs);
    free(alloc_samples);
    free(insert_samples);
    return 0;
}
