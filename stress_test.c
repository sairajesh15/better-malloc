#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define STRESS_ARENA_SIZE (64 * 1024 * 1024) /* 64 MB arena */
#define MAX_ACTIVE_BLOCKS 8192

#ifdef STRESS_DEBUG
#define DEFAULT_STRESS_OPS 10000
#else
#define DEFAULT_STRESS_OPS 1000000
#endif

typedef struct {
    void *ptr;
    size_t size;
    uint8_t seed;
} shadow_block_t;

static shadow_block_t shadow[MAX_ACTIVE_BLOCKS];
static size_t active_indices[MAX_ACTIVE_BLOCKS];
static size_t active_count = 0;

/* Fast Pseudo-Random Generator (Xorshift32) for fast, portable reproducibility */
static uint32_t g_rng_state = 1337;

static inline uint32_t fast_rand(void) {
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

/*
 * Two distributions:
 * 1. Small-Heavy: 80% [1..128 B], 15% [129..1024 B], 5% [1025..8192 B]
 * 2. Mixed:       40% [1..512 B], 35% [513..2048 B], 25% [2049..8192 B]
 */
static size_t pick_size(int dist_mode) {
    uint32_t roll = fast_rand() % 100;
    if (dist_mode == 0) {
        /* Small-Heavy */
        if (roll < 80) {
            return (fast_rand() % 128) + 1;
        } else if (roll < 95) {
            return 129 + (fast_rand() % (1024 - 128));
        } else {
            return 1025 + (fast_rand() % (8192 - 1024));
        }
    } else {
        /* Mixed */
        if (roll < 40) {
            return (fast_rand() % 512) + 1;
        } else if (roll < 75) {
            return 513 + (fast_rand() % (2048 - 512));
        } else {
            return 2049 + (fast_rand() % (8192 - 2049));
        }
    }
}

int main(int argc, char *argv[]) {
    unsigned int seed = 1337;
    if (argc > 1) {
        seed = (unsigned int)strtoul(argv[1], NULL, 0);
    }
    g_rng_state = (seed == 0) ? 1337 : seed;

    int total_ops = DEFAULT_STRESS_OPS;
    if (argc > 2) {
        total_ops = atoi(argv[2]);
        if (total_ops <= 0) total_ops = DEFAULT_STRESS_OPS;
    }

    printf("================================================================================\n");
    printf("            PROJECT 23: RANDOMIZED STRESS TEST & INTEGRITY AUDIT               \n");
    printf("================================================================================\n");
    printf("Configuration:\n");
    printf("  - PRNG Seed            : %u\n", seed);
    printf("  - Total Operations     : %d\n", total_ops);
    printf("  - Arena Capacity       : %d MB (%d bytes)\n", STRESS_ARENA_SIZE / (1024 * 1024), STRESS_ARENA_SIZE);
    printf("  - Max Active Blocks    : %d\n", MAX_ACTIVE_BLOCKS);
    printf("  - Distributions        : Phase 1 (Small-Heavy) -> Phase 2 (Mixed)\n");
#ifdef STRESS_DEBUG
    printf("  - Mem_Check Mode       : DEBUG (Exhaustive validation after EVERY operation)\n");
#else
    printf("  - Mem_Check Mode       : FAST (Exhaustive validation every 1,000 operations)\n");
#endif
    printf("--------------------------------------------------------------------------------\n\n");

    int res = Mem_Init(STRESS_ARENA_SIZE);
    assert(res == 0);
    assert(Mem_Check() == 0);

    size_t alloc_successes = 0;
    size_t alloc_failures = 0;
    size_t frees_performed = 0;

    int half_ops = total_ops / 2;
    int progress_interval = (total_ops >= 100000) ? 100000 : 1000;

    for (int op = 0; op < total_ops; op++) {
        int dist_mode = (op < half_ops) ? 0 : 1;

        /* Decide Action: 0 = Free, 1 = Alloc */
        int action = 1;
        if (active_count == MAX_ACTIVE_BLOCKS) {
            action = 0; /* Force free when shadow table is full */
        } else if (active_count == 0) {
            action = 1; /* Force alloc when shadow table is empty */
        } else {
            action = (fast_rand() % 100 < 50) ? 1 : 0;
        }

        if (action == 1) {
            /* ALLOCATE */
            size_t req_sz = pick_size(dist_mode);
            void *p = Mem_Alloc((int)req_sz);
            if (p != NULL) {
                /* Strict 16-byte alignment requirement */
                assert(((uintptr_t)p & 15) == 0);

                uint8_t byte_seed = (uint8_t)(fast_rand() & 0xFF);
                uint8_t *bptr = (uint8_t *)p;
                for (size_t b = 0; b < req_sz; b++) {
                    bptr[b] = (uint8_t)((byte_seed + b) & 0xFF);
                }

                /* Find empty slot in shadow table */
                size_t slot = 0;
                while (slot < MAX_ACTIVE_BLOCKS && shadow[slot].ptr != NULL) {
                    slot++;
                }
                assert(slot < MAX_ACTIVE_BLOCKS);

                shadow[slot].ptr = p;
                shadow[slot].size = req_sz;
                shadow[slot].seed = byte_seed;

                active_indices[active_count] = slot;
                active_count++;
                alloc_successes++;
            } else {
                alloc_failures++;
                /* If arena is full, proactively free a block to make room */
                if (active_count > 0) {
                    size_t rand_idx = fast_rand() % active_count;
                    size_t slot = active_indices[rand_idx];
                    shadow_block_t *entry = &shadow[slot];

                    uint8_t *bptr = (uint8_t *)entry->ptr;
                    for (size_t b = 0; b < entry->size; b++) {
                        uint8_t expected = (uint8_t)((entry->seed + b) & 0xFF);
                        assert(bptr[b] == expected);
                    }

                    assert(Mem_Free(entry->ptr) == 0);
                    entry->ptr = NULL;

                    active_indices[rand_idx] = active_indices[active_count - 1];
                    active_count--;
                    frees_performed++;
                }
            }
        } else {
            /* FREE */
            size_t rand_idx = fast_rand() % active_count;
            size_t slot = active_indices[rand_idx];
            shadow_block_t *entry = &shadow[slot];

            uint8_t *bptr = (uint8_t *)entry->ptr;
            for (size_t b = 0; b < entry->size; b++) {
                uint8_t expected = (uint8_t)((entry->seed + b) & 0xFF);
                assert(bptr[b] == expected);
            }

            assert(Mem_Free(entry->ptr) == 0);
            entry->ptr = NULL;

            active_indices[rand_idx] = active_indices[active_count - 1];
            active_count--;
            frees_performed++;
        }

        /* Continuous Heap Validation */
#ifdef STRESS_DEBUG
        assert(Mem_Check() == 0);
#else
        if ((op + 1) % 1000 == 0) {
            assert(Mem_Check() == 0);
        }
#endif

        if ((op + 1) % progress_interval == 0 || (op + 1) == total_ops) {
            const char *dist_str = (dist_mode == 0) ? "Small-Heavy" : "Mixed";
            printf("  [OP %7d / %7d] (%-11s) Active: %4zu | Allocs: %6zu (Fail: %zu) | Frees: %6zu | Mem_Check: OK\n",
                   op + 1, total_ops, dist_str, active_count, alloc_successes, alloc_failures, frees_performed);
            fflush(stdout);
        }
    }

    printf("\n>>> Operations complete. Draining all %zu remaining active blocks... <<<\n", active_count);
    while (active_count > 0) {
        size_t slot = active_indices[active_count - 1];
        shadow_block_t *entry = &shadow[slot];

        uint8_t *bptr = (uint8_t *)entry->ptr;
        for (size_t b = 0; b < entry->size; b++) {
            uint8_t expected = (uint8_t)((entry->seed + b) & 0xFF);
            assert(bptr[b] == expected);
        }

        assert(Mem_Free(entry->ptr) == 0);
        entry->ptr = NULL;
        active_count--;
    }

    /* Validate complete heap consistency */
    assert(Mem_Check() == 0);
    printf("[PASS] All blocks successfully verified and freed. Mem_Check() == 0.\n");

    /* Assert single free block: Allocating maximum arena capacity must succeed */
    size_t max_payload = Mem_GetArenaSize() - 48 - 8;
    void *giant = Mem_Alloc((int)max_payload);
    assert(giant != NULL);
    printf("[PASS] Arena completely coalesced into a single free block (%zu B usable payload allocated)!\n", max_payload);
    printf("       [METRIC EXPLANATION]: %zu bytes is the maximum usable user payload (arena_size - 48 header - 8 canary).\n", max_payload);
    printf("       The underlying free block capacity is exactly %zu bytes (arena_size - 48 header).\n", max_payload + 8);

    assert(Mem_Free(giant) == 0);
    assert(Mem_Check() == 0);

    /* ========================================================================= */
    /* PHASE 3: 1 MB High-Pressure Saturation Phase (85% - 95% Live Utilization)  */
    /* ========================================================================= */
    printf("\n--------------------------------------------------------------------------------\n");
    printf(">>> STARTING PHASE 3: 1 MB HIGH-PRESSURE SATURATION PHASE (85%% - 95%% LIVE) <<<\n");
    printf("--------------------------------------------------------------------------------\n");
    Mem_Reset();

    size_t pressure_arena_size = 1024 * 1024; /* 1 MB arena */
    res = Mem_Init((int)pressure_arena_size);
    assert(res == 0);
    assert(Mem_Check() == 0);

    size_t target_low = (size_t)(pressure_arena_size * 0.85);
    size_t target_high = (size_t)(pressure_arena_size * 0.95);

#ifdef STRESS_DEBUG
    int pressure_ops = 5000;
#else
    int pressure_ops = 50000;
#endif

    size_t p_active_count = 0;
    size_t p_live_bytes = 0;
    size_t p_alloc_no_space = 0;
    size_t p_alloc_success = 0;
    size_t p_frees = 0;

    for (int op = 0; op < pressure_ops; op++) {
        int action = 1;
        if (p_active_count == MAX_ACTIVE_BLOCKS || p_live_bytes >= target_high) {
            action = 0; /* Force free when near 95% threshold */
        } else if (p_active_count == 0 || p_live_bytes < target_low) {
            action = 1; /* Force alloc when below 85% threshold */
        } else {
            action = (fast_rand() % 100 < 50) ? 1 : 0;
        }

        if (action == 1) {
            size_t req_sz = 64 + (fast_rand() % 448);
            void *p = Mem_Alloc((int)req_sz);
            if (p != NULL) {
                assert(((uintptr_t)p & 15) == 0);
                uint8_t byte_seed = (uint8_t)(fast_rand() & 0xFF);
                uint8_t *bptr = (uint8_t *)p;
                for (size_t b = 0; b < req_sz; b++) {
                    bptr[b] = (uint8_t)((byte_seed + b) & 0xFF);
                }

                size_t slot = 0;
                while (slot < MAX_ACTIVE_BLOCKS && shadow[slot].ptr != NULL) slot++;
                assert(slot < MAX_ACTIVE_BLOCKS);

                shadow[slot].ptr = p;
                shadow[slot].size = req_sz;
                shadow[slot].seed = byte_seed;

                active_indices[p_active_count] = slot;
                p_active_count++;
                p_live_bytes += req_sz;
                p_alloc_success++;
            } else {
                /* Gracefully handle E_NO_SPACE under intense memory pressure */
                assert(m_error == E_NO_SPACE);
                p_alloc_no_space++;
                if (p_active_count > 0) {
                    size_t rand_idx = fast_rand() % p_active_count;
                    size_t slot = active_indices[rand_idx];
                    shadow_block_t *entry = &shadow[slot];

                    uint8_t *bptr = (uint8_t *)entry->ptr;
                    for (size_t b = 0; b < entry->size; b++) {
                        assert(bptr[b] == (uint8_t)((entry->seed + b) & 0xFF));
                    }

                    p_live_bytes -= entry->size;
                    assert(Mem_Free(entry->ptr) == 0);
                    entry->ptr = NULL;

                    active_indices[rand_idx] = active_indices[p_active_count - 1];
                    p_active_count--;
                    p_frees++;
                }
            }
        } else {
            size_t rand_idx = fast_rand() % p_active_count;
            size_t slot = active_indices[rand_idx];
            shadow_block_t *entry = &shadow[slot];

            uint8_t *bptr = (uint8_t *)entry->ptr;
            for (size_t b = 0; b < entry->size; b++) {
                assert(bptr[b] == (uint8_t)((entry->seed + b) & 0xFF));
            }

            p_live_bytes -= entry->size;
            assert(Mem_Free(entry->ptr) == 0);
            entry->ptr = NULL;

            active_indices[rand_idx] = active_indices[p_active_count - 1];
            p_active_count--;
            p_frees++;
        }

#ifdef STRESS_DEBUG
        assert(Mem_Check() == 0);
#else
        if ((op + 1) % 1000 == 0) assert(Mem_Check() == 0);
#endif

        if ((op + 1) % ((pressure_ops >= 10000) ? 10000 : 1000) == 0) {
            double util_pct = ((double)p_live_bytes / (double)pressure_arena_size) * 100.0;
            printf("  [PRESSURE OP %5d / %5d] Live: %6zu B (%.1f%% Util) | Active: %4zu | E_NO_SPACE Handled: %zu | Mem_Check: OK\n",
                   op + 1, pressure_ops, p_live_bytes, util_pct, p_active_count, p_alloc_no_space);
            fflush(stdout);
        }
    }

    printf(">>> Draining remaining %zu blocks from 1 MB pressure arena... <<<\n", p_active_count);
    while (p_active_count > 0) {
        size_t slot = active_indices[p_active_count - 1];
        shadow_block_t *entry = &shadow[slot];

        uint8_t *bptr = (uint8_t *)entry->ptr;
        for (size_t b = 0; b < entry->size; b++) {
            assert(bptr[b] == (uint8_t)((entry->seed + b) & 0xFF));
        }

        assert(Mem_Free(entry->ptr) == 0);
        entry->ptr = NULL;
        p_active_count--;
    }

    assert(Mem_Check() == 0);

    /* Assert exactly one single free block whose size == arena_size - 48 */
    typedef struct {
        size_t size;
        int is_free;
        uint32_t magic;
        void *prev_phys;
        void *next_phys;
        void *prev_free;
        void *next_free;
    } test_header_t;

    size_t expected_block_size = pressure_arena_size - 48;
    test_header_t *p_head = (test_header_t *)Mem_GetArenaStart();
    assert(p_head != NULL);
    assert(p_head->is_free == 1);
    assert(p_head->size == expected_block_size);
    assert(p_head->prev_phys == NULL);
    assert(p_head->next_phys == NULL);
    printf("[PASS] Pressure Phase: Successfully asserted 1 free block of size == arena_size - 48 (%zu bytes) [Allocs: %zu, Frees: %zu]!\n",
           expected_block_size, p_alloc_success, p_frees);

    printf("\n================================================================================\n");
    printf("STRESS TEST RESULT: 100%% SUCCESS (%d Primary Ops + %d Pressure Ops, Seed: %u)\n",
           total_ops, pressure_ops, seed);
    printf("================================================================================\n\n");

    return 0;
}
