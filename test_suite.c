#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <limits.h>

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "\n[ASSERTION FAILED] %s:%d: %s (Condition: %s)\n", \
                __FILE__, __LINE__, msg, #cond); \
        exit(1); \
    } \
} while (0)

/* ========================================================================= */
/* Test 1: Uninitialized Calls and Bad Arguments                             */
/* ========================================================================= */
static void test_uninitialized_and_bad_args(void) {
    m_error = 0;
    void *p = Mem_Alloc(64);
    TEST_ASSERT(p == NULL, "Mem_Alloc before Mem_Init must return NULL");
    TEST_ASSERT(m_error == E_BAD_ARGS, "m_error must be E_BAD_ARGS before Mem_Init");

    m_error = 0;
    int res = Mem_Free((void *)0x12345678);
    TEST_ASSERT(res == -1, "Mem_Free before Mem_Init must return -1");
    TEST_ASSERT(m_error == E_BAD_POINTER, "m_error must be E_BAD_POINTER");

    m_error = 0;
    res = Mem_Free(NULL);
    TEST_ASSERT(res == 0, "Mem_Free(NULL) must return 0");

    Mem_Dump();

    m_error = 0;
    res = Mem_Init(0);
    TEST_ASSERT(res == -1, "Mem_Init(0) must return -1");
    TEST_ASSERT(m_error == E_BAD_ARGS, "Mem_Init(0) must set m_error = E_BAD_ARGS");

    m_error = 0;
    res = Mem_Init(-4096);
    TEST_ASSERT(res == -1, "Mem_Init(-4096) must return -1");
    TEST_ASSERT(m_error == E_BAD_ARGS, "Mem_Init(-4096) must set m_error = E_BAD_ARGS");
}

/* ========================================================================= */
/* Test 2: Single mmap Page-Rounding, Double Init & Syscall Counter          */
/* ========================================================================= */
static void test_page_rounding_and_double_init(void) {
    int page_size = getpagesize();
    TEST_ASSERT(page_size > 0, "getpagesize() must be positive");

    int res = Mem_Init(1);
    TEST_ASSERT(res == 0, "Mem_Init(1) should succeed and round up to 1 page");
    TEST_ASSERT(m_error == 0, "m_error should be 0 on success");
    TEST_ASSERT(Mem_GetMmapCount() == 1, "mmap syscall counter must be exactly 1");

    m_error = 0;
    res = Mem_Init(page_size);
    TEST_ASSERT(res == -1, "Second Mem_Init call must return -1");
    TEST_ASSERT(m_error == E_BAD_ARGS, "Second Mem_Init call must set m_error = E_BAD_ARGS");

    m_error = 0;
    res = Mem_Init(page_size * 2);
    TEST_ASSERT(res == -1, "Third Mem_Init call must return -1");
    TEST_ASSERT(m_error == E_BAD_ARGS, "Third Mem_Init call must set m_error = E_BAD_ARGS");
    TEST_ASSERT(Mem_GetMmapCount() == 1, "mmap syscall counter must still be 1");

    int max_payload = page_size - 48 - 8;
    void *p = Mem_Alloc(max_payload);
    TEST_ASSERT(p != NULL, "Allocating max payload in 1 page arena should succeed");

    m_error = 0;
    void *fail_p = Mem_Alloc(8);
    TEST_ASSERT(fail_p == NULL, "Allocating when arena is full must return NULL");
    TEST_ASSERT(m_error == E_NO_SPACE, "m_error must be E_NO_SPACE when full");

    res = Mem_Free(p);
    TEST_ASSERT(res == 0, "Freeing max payload block should succeed");
}

/* ========================================================================= */
/* Test 3: Strict 8-Byte Pointer Alignment Across Odd Sizes & Canary Arming  */
/* ========================================================================= */
#define NUM_TEST_SIZES 29

static void test_pointer_alignment_and_integrity(void) {
    int res = Mem_Init(65536);
    TEST_ASSERT(res == 0, "Mem_Init(65536) must succeed");

    static const int sizes[NUM_TEST_SIZES] = {
        1, 2, 3, 5, 7, 8, 9, 11, 13, 15, 17, 21, 29, 31, 33, 47, 49,
        63, 65, 71, 97, 127, 128, 255, 257, 511, 513, 1023, 1025
    };
    void *ptrs[NUM_TEST_SIZES];

    for (size_t i = 0; i < NUM_TEST_SIZES; i++) {
        ptrs[i] = Mem_Alloc(sizes[i]);
        TEST_ASSERT(ptrs[i] != NULL, "Mem_Alloc must not return NULL");

        uintptr_t addr = (uintptr_t)ptrs[i];
        TEST_ASSERT((addr & 7) == 0, "Payload pointer must be strictly 8-byte aligned");
        TEST_ASSERT((addr & 15) == 0, "Payload pointer must be strictly 16-byte aligned (MEM_ALIGN=16)");

        /* Fill memory within user payload bounds (all sizes[i] bytes safe before trailing canary) */
        size_t write_len = (size_t)sizes[i];
        uint8_t pattern = (uint8_t)((i * 37 + sizes[i]) & 0xFF);
        memset(ptrs[i], pattern, write_len);
    }

    for (size_t i = 0; i < NUM_TEST_SIZES; i++) {
        size_t write_len = (size_t)sizes[i];
        uint8_t expected = (uint8_t)((i * 37 + sizes[i]) & 0xFF);
        uint8_t *byte_ptr = (uint8_t *)ptrs[i];
        for (size_t b = 0; b < write_len; b++) {
            TEST_ASSERT(byte_ptr[b] == expected, "Payload memory corrupted");
        }
    }

    /* Free all pointers, verifying canary integrity */
    for (size_t i = 0; i < NUM_TEST_SIZES; i++) {
        res = Mem_Free(ptrs[i]);
        TEST_ASSERT(res == 0, "Mem_Free must succeed and canary must be valid");
    }
}

/* ========================================================================= */
/* Test 4: Canary Buffer Overflow Detection (E_PADDING_OVERWRITTEN)          */
/* ========================================================================= */
static void test_canary_buffer_overflow_detection(void) {
    int res = Mem_Init(32768);
    TEST_ASSERT(res == 0, "Mem_Init must succeed");

    /* Allocate 128 bytes */
    void *p = Mem_Alloc(128);
    TEST_ASSERT(p != NULL, "Mem_Alloc(128) must succeed");

    /* Normal free succeeds */
    TEST_ASSERT(Mem_Free(p) == 0, "Normal free must succeed");

    /* Allocate again and deliberately overwrite the trailing canary */
    void *p2 = Mem_Alloc(128);
    TEST_ASSERT(p2 != NULL, "Mem_Alloc(128) must succeed");

    /* Canary is immediately following requested payload at p2 + 128 */
    uint64_t *canary_location = (uint64_t *)((char *)p2 + 128);
    *canary_location = 0x4141414141414141ULL; /* Corrupted canary */

    m_error = 0;
    res = Mem_Free(p2);
    TEST_ASSERT(res == -1, "Freeing buffer with corrupted canary must return -1");
    TEST_ASSERT(m_error == E_PADDING_OVERWRITTEN, "m_error must be E_PADDING_OVERWRITTEN (3)");

    /* Restore canary so we can clean up cleanly */
    *canary_location = CANARY_MAGIC;
    TEST_ASSERT(Mem_Free(p2) == 0, "Clean up after restoring canary");

    /* Test slack padding byte (0xA5) corruption: allocate 17 bytes */
    void *p3 = Mem_Alloc(17);
    TEST_ASSERT(p3 != NULL, "Mem_Alloc(17) must succeed");

    /* Canary occupies 8 bytes at p3 + 17; padding byte starts at p3 + 17 + 8 */
    char *pad_location = (char *)p3 + 17 + 8;
    *pad_location = 0x5A; /* Corrupted padding byte */

    m_error = 0;
    res = Mem_Free(p3);
    TEST_ASSERT(res == -1, "Freeing buffer with corrupted padding byte must return -1");
    TEST_ASSERT(m_error == E_PADDING_OVERWRITTEN, "m_error must be E_PADDING_OVERWRITTEN (3)");

    /* Restore padding byte to clean up */
    *pad_location = (char)CANARY_PAD_BYTE;
    TEST_ASSERT(Mem_Free(p3) == 0, "Clean up after restoring padding byte");
}

/* ========================================================================= */
/* Test 5: Block Splitting and Segregated Bin Placement                      */
/* ========================================================================= */
static void test_block_splitting_and_bins(void) {
    int page_size = getpagesize();
    int res = Mem_Init(page_size);
    TEST_ASSERT(res == 0, "Mem_Init must succeed");

    void *p1 = Mem_Alloc(64);
    TEST_ASSERT(p1 != NULL, "Allocating 64 bytes should succeed");

    void *p2 = Mem_Alloc(64);
    TEST_ASSERT(p2 != NULL, "Allocating 64 bytes from remainder should succeed");

    TEST_ASSERT(Mem_Free(p2) == 0, "Free p2");
    TEST_ASSERT(Mem_Free(p1) == 0, "Free p1");

    /* Near-boundary split: remainder must have 48 (header) + 16 (min payload+canary) = 64 */
    int req1 = page_size - 48 - 64 - 8;
    void *p_exact = Mem_Alloc(req1);
    TEST_ASSERT(p_exact != NULL, "Exact split boundary alloc");

    void *p_rem = Mem_Alloc(8);
    TEST_ASSERT(p_rem != NULL, "8-byte remainder alloc");

    TEST_ASSERT(Mem_Free(p_exact) == 0, "Free p_exact");
    TEST_ASSERT(Mem_Free(p_rem) == 0, "Free p_rem");
}

/* ========================================================================= */
/* Test 6: Coalescing Cases (None, Next, Prev, Both) & Full Restoration      */
/* ========================================================================= */
static void test_coalescing_cases(void) {
    int page_size = getpagesize();
    int res = Mem_Init(page_size);
    TEST_ASSERT(res == 0, "Mem_Init must succeed");

    size_t total_usable = page_size - 48 - 8;

    void *a = Mem_Alloc(128);
    void *b = Mem_Alloc(128);
    void *c = Mem_Alloc(128);
    TEST_ASSERT(a && b && c, "All 3 allocations must succeed");

    /* Case 1: Free B (neither neighbor free) */
    res = Mem_Free(b);
    TEST_ASSERT(res == 0, "Mem_Free(b) should succeed");

    /* Case 2: Free C (merges with B and remainder D) */
    res = Mem_Free(c);
    TEST_ASSERT(res == 0, "Mem_Free(c) should succeed");

    /* Case 3: Free A (merges with B+C+D) */
    res = Mem_Free(a);
    TEST_ASSERT(res == 0, "Mem_Free(a) should succeed");

    /* Entire arena restored to 1 block */
    void *full_block = Mem_Alloc((int)total_usable);
    TEST_ASSERT(full_block != NULL, "Full arena allocation must succeed");
    TEST_ASSERT(Mem_Free(full_block) == 0, "Free full block");

    /* Case 4: Free middle block when both left and right are already free */
    void *x = Mem_Alloc(128);
    void *y = Mem_Alloc(128);
    void *z = Mem_Alloc(128);
    void *tail = Mem_Alloc(128);
    TEST_ASSERT(x && y && z && tail, "Alloc x, y, z, tail");

    TEST_ASSERT(Mem_Free(x) == 0, "Free x");
    TEST_ASSERT(Mem_Free(z) == 0, "Free z");
    TEST_ASSERT(Mem_Free(y) == 0, "Free y with both free");

    void *merged = Mem_Alloc(480);
    TEST_ASSERT(merged == x, "Merged allocation must reuse address of x");

    TEST_ASSERT(Mem_Free(merged) == 0, "Free merged");
    TEST_ASSERT(Mem_Free(tail) == 0, "Free tail");

    full_block = Mem_Alloc((int)total_usable);
    TEST_ASSERT(full_block != NULL, "Full arena restored");
    TEST_ASSERT(Mem_Free(full_block) == 0, "Clean up full block");
}

/* ========================================================================= */
/* Test 7: Interior Pointers and Error Codes                                 */
/* ========================================================================= */
static void test_interior_pointers_and_bad_pointers(void) {
    int page_size = getpagesize();
    int res = Mem_Init(page_size);
    TEST_ASSERT(res == 0, "Mem_Init must succeed");

    void *p1 = Mem_Alloc(256);
    TEST_ASSERT(p1 != NULL, "Mem_Alloc(256) must succeed");

#if MEM_ALLOW_INTERIOR
    res = Mem_Free((char *)p1 + 24);
    TEST_ASSERT(res == 0, "Interior pointer (+24 B) free must succeed when MEM_ALLOW_INTERIOR=1");

    m_error = 0;
    res = Mem_Free(p1);
    TEST_ASSERT(res == -1, "Double-free must fail");
    TEST_ASSERT(m_error == E_BAD_POINTER, "Must set E_BAD_POINTER on double free");
#else
    res = Mem_Free((char *)p1 + 24);
    TEST_ASSERT(res == -1, "Interior pointer (+24 B) free must fail when MEM_ALLOW_INTERIOR=0");
    TEST_ASSERT(m_error == E_BAD_POINTER, "Must set E_BAD_POINTER on interior pointer when MEM_ALLOW_INTERIOR=0");

    res = Mem_Free(p1);
    TEST_ASSERT(res == 0, "Exact pointer free must succeed when MEM_ALLOW_INTERIOR=0");

    m_error = 0;
    res = Mem_Free(p1);
    TEST_ASSERT(res == -1, "Double-free of exact pointer must fail");
    TEST_ASSERT(m_error == E_BAD_POINTER, "Must set E_BAD_POINTER on double free");
#endif

    void *p2 = Mem_Alloc(128);
    TEST_ASSERT(p2 != NULL, "Mem_Alloc(128)");

    m_error = 0;
    res = Mem_Free((char *)p2 - 8);
    TEST_ASSERT(res == -1, "Header pointer must return -1");
    TEST_ASSERT(m_error == E_BAD_POINTER, "Must set E_BAD_POINTER");

    m_error = 0;
    res = Mem_Free((char *)p2 + 1000000);
    TEST_ASSERT(res == -1, "Out of bounds pointer");
    TEST_ASSERT(m_error == E_BAD_POINTER, "Must set E_BAD_POINTER");

    m_error = 0;
    res = Mem_Free(NULL);
    TEST_ASSERT(res == 0, "Mem_Free(NULL) must return 0");
    TEST_ASSERT(m_error == 0, "m_error unchanged");

    TEST_ASSERT(Mem_Free(p2) == 0, "Clean up p2");
}

/* ========================================================================= */
/* Test 8: Alternating Fragmentation and First-Fit Bin Reuse                 */
/* ========================================================================= */
#define NUM_FRAG_BLOCKS 32

static void test_fragmentation_and_first_fit(void) {
    int res = Mem_Init(131072);
    TEST_ASSERT(res == 0, "Mem_Init must succeed");

    void *ptrs[NUM_FRAG_BLOCKS];

    for (int i = 0; i < NUM_FRAG_BLOCKS; i++) {
        ptrs[i] = Mem_Alloc(256);
        TEST_ASSERT(ptrs[i] != NULL, "Allocating 256 bytes");
    }

    for (int i = 0; i < NUM_FRAG_BLOCKS; i += 2) {
        TEST_ASSERT(Mem_Free(ptrs[i]) == 0, "Freeing even block");
    }

    void *new_ptrs[NUM_FRAG_BLOCKS / 2];
    for (int i = 0; i < NUM_FRAG_BLOCKS / 2; i++) {
        new_ptrs[i] = Mem_Alloc(128);
        TEST_ASSERT(new_ptrs[i] != NULL, "Allocating into holes");
        TEST_ASSERT(new_ptrs[i] == ptrs[i * 2], "First-Fit should choose lowest available hole");
    }

    for (int i = 0; i < NUM_FRAG_BLOCKS / 2; i++) {
        TEST_ASSERT(Mem_Free(new_ptrs[i]) == 0, "Free new ptr");
    }

    for (int i = 1; i < NUM_FRAG_BLOCKS; i += 2) {
        TEST_ASSERT(Mem_Free(ptrs[i]) == 0, "Free odd block");
    }

    void *huge = Mem_Alloc(131072 - 48 - 8);
    TEST_ASSERT(huge != NULL, "Must coalesce completely");
    TEST_ASSERT(Mem_Free(huge) == 0, "Clean up huge block");
}

/* ========================================================================= */
/* Test 9: Mem_Check Exhaustive Heap Consistency and Fault Invariant Checker */
/* ========================================================================= */
static void test_mem_check_integrity(void) {
    /* 1. Mem_Check before Mem_Init must fail */
    TEST_ASSERT(Mem_Check() == -1, "Mem_Check before Mem_Init must return -1");

    int res = Mem_Init(65536);
    TEST_ASSERT(res == 0, "Mem_Init(65536) must succeed");

    /* 2. Pristine arena must pass Mem_Check */
    TEST_ASSERT(Mem_Check() == 0, "Initial clean arena must pass Mem_Check");

    /* 3. Mixed allocations */
    void *p1 = Mem_Alloc(32);
    void *p2 = Mem_Alloc(64);
    void *p3 = Mem_Alloc(128);
    void *p4 = Mem_Alloc(256);
    TEST_ASSERT(p1 && p2 && p3 && p4, "All 4 allocations must succeed");
    TEST_ASSERT(Mem_Check() == 0, "Allocated state must pass Mem_Check");

    /* 4. Partial deallocation exercising bin insertion */
    TEST_ASSERT(Mem_Free(p2) == 0, "Mem_Free(p2)");
    TEST_ASSERT(Mem_Check() == 0, "Partial free state must pass Mem_Check");

    /* 5. Intentionally corrupt block magic on p3's header (offset -48 + 12) */
    uint32_t *p3_magic = (uint32_t *)((char *)p3 - 48 + 12);
    uint32_t orig_magic = *p3_magic;
    *p3_magic = 0xDEADBEEF;
    TEST_ASSERT(Mem_Check() == -1, "Mem_Check must detect corrupted magic tag");
    *p3_magic = orig_magic;
    TEST_ASSERT(Mem_Check() == 0, "Mem_Check must pass after restoring magic tag");

    /* 6. Intentionally corrupt trailing canary on p4 */
    uint64_t *p4_canary = (uint64_t *)((char *)p4 + 256);
    uint64_t orig_canary = *p4_canary;
    *p4_canary = 0xCAFEBABE00000000ULL;
    TEST_ASSERT(Mem_Check() == -1, "Mem_Check must detect corrupted canary");
    *p4_canary = orig_canary;
    TEST_ASSERT(Mem_Check() == 0, "Mem_Check must pass after restoring canary");

    /* 7. Clean up all allocations */
    TEST_ASSERT(Mem_Free(p1) == 0, "Free p1");
    TEST_ASSERT(Mem_Free(p3) == 0, "Free p3");
    TEST_ASSERT(Mem_Free(p4) == 0, "Free p4");

    /* 8. Coalesced arena must pass Mem_Check */
    TEST_ASSERT(Mem_Check() == 0, "Fully coalesced arena must pass Mem_Check");
}

/* ========================================================================= */
/* Test 10: Randomized Churn Stress Test with Continuous Mem_Check Validation*/
/* ========================================================================= */
#define STRESS_ARENA_SIZE   262144  /* 256 KB */
#define MAX_ACTIVE_ALLOCS   128
#define STRESS_ITERATIONS   5000

typedef struct {
    void *ptr;
    size_t size;
    uint8_t seed;
} active_record_t;

static void test_randomized_stress(void) {
    int res = Mem_Init(STRESS_ARENA_SIZE);
    TEST_ASSERT(res == 0, "Mem_Init(STRESS_ARENA_SIZE) must succeed");
    TEST_ASSERT(Mem_Check() == 0, "Initial Mem_Check must pass");

    active_record_t active[MAX_ACTIVE_ALLOCS];
    for (int i = 0; i < MAX_ACTIVE_ALLOCS; i++) {
        active[i].ptr = NULL;
        active[i].size = 0;
        active[i].seed = 0;
    }

    size_t active_count = 0;
    uint32_t lcg = 0x12345678;

    #define NEXT_RAND() (lcg = lcg * 1664525U + 1013904223U)

    for (int iter = 0; iter < STRESS_ITERATIONS; iter++) {
        int should_alloc = (active_count == 0) || ((NEXT_RAND() % 100) < 55 && active_count < MAX_ACTIVE_ALLOCS);

        if (should_alloc) {
            /* Find an empty slot */
            int slot = -1;
            for (int i = 0; i < MAX_ACTIVE_ALLOCS; i++) {
                if (active[i].ptr == NULL) {
                    slot = i;
                    break;
                }
            }
            TEST_ASSERT(slot >= 0, "Empty slot must exist");

            /* Random size from 1 to 1024 bytes */
            size_t req_sz = (NEXT_RAND() % 1024) + 1;
            void *p = Mem_Alloc((int)req_sz);
            if (p != NULL) {
                TEST_ASSERT(((uintptr_t)p & 15) == 0, "Allocation must satisfy 16-byte alignment");
                uint8_t seed = (uint8_t)(NEXT_RAND() & 0xFF);
                /* Write deterministic verification pattern */
                uint8_t *bptr = (uint8_t *)p;
                for (size_t b = 0; b < req_sz; b++) {
                    bptr[b] = (uint8_t)((seed + b) & 0xFF);
                }
                active[slot].ptr = p;
                active[slot].size = req_sz;
                active[slot].seed = seed;
                active_count++;
            }
        } else {
            /* Pick a random active allocation to verify and free */
            int slot = -1;
            int start_idx = (int)(NEXT_RAND() % MAX_ACTIVE_ALLOCS);
            for (int k = 0; k < MAX_ACTIVE_ALLOCS; k++) {
                int idx = (start_idx + k) % MAX_ACTIVE_ALLOCS;
                if (active[idx].ptr != NULL) {
                    slot = idx;
                    break;
                }
            }
            TEST_ASSERT(slot >= 0, "Active slot must exist");

            /* Verify byte integrity before freeing */
            uint8_t *bptr = (uint8_t *)active[slot].ptr;
            for (size_t b = 0; b < active[slot].size; b++) {
                uint8_t expected = (uint8_t)((active[slot].seed + b) & 0xFF);
                TEST_ASSERT(bptr[b] == expected, "Payload corrupted during churn");
            }

            res = Mem_Free(active[slot].ptr);
            TEST_ASSERT(res == 0, "Mem_Free must succeed during stress churn");

            active[slot].ptr = NULL;
            active[slot].size = 0;
            active[slot].seed = 0;
            active_count--;
        }

        /* Periodic continuous heap invariant validation */
        if (iter % 500 == 0) {
            TEST_ASSERT(Mem_Check() == 0, "Continuous Mem_Check failed during stress test");
        }
    }

    /* Free all remaining active allocations and verify contents */
    for (int i = 0; i < MAX_ACTIVE_ALLOCS; i++) {
        if (active[i].ptr != NULL) {
            uint8_t *bptr = (uint8_t *)active[i].ptr;
            for (size_t b = 0; b < active[i].size; b++) {
                uint8_t expected = (uint8_t)((active[i].seed + b) & 0xFF);
                TEST_ASSERT(bptr[b] == expected, "Final active payload corrupted");
            }
            res = Mem_Free(active[i].ptr);
            TEST_ASSERT(res == 0, "Final cleanup free must succeed");
            active[i].ptr = NULL;
        }
    }

    /* Heap must pass Mem_Check and be 100% coalesced back into a single block */
    TEST_ASSERT(Mem_Check() == 0, "Mem_Check after draining all active blocks must succeed");

    void *all_restored = Mem_Alloc((int)(STRESS_ARENA_SIZE - 48 - 8));
    TEST_ASSERT(all_restored != NULL, "Arena must completely coalesce back to 1 large block");
    TEST_ASSERT(Mem_Free(all_restored) == 0, "Clean up full arena block");
    TEST_ASSERT(Mem_Check() == 0, "Final Mem_Check must succeed");
}

/* ========================================================================= */
/* Test 11: Negative Test Suite (One Line Per Case)                          */
/* ========================================================================= */
static void run_negative_case_overflow(int size) {
    void *p = Mem_Alloc(size);
    TEST_ASSERT(p != NULL, "Alloc must succeed");
    /* The canary is placed at ((char *)p + size). Corrupting the first byte of canary */
    char *canary_byte = (char *)p + size;
    *canary_byte ^= 0x55;
    m_error = 0;
    int rc = Mem_Free(p);
    TEST_ASSERT(rc == -1, "Overwritten canary free must return -1");
    TEST_ASSERT(m_error == E_PADDING_OVERWRITTEN, "m_error must be E_PADDING_OVERWRITTEN");
    printf("[NEGATIVE TEST] 1-byte overflow at size %-3d                 : CAUGHT (rc = %d, m_error = %d [E_PADDING_OVERWRITTEN])\n",
           size, rc, m_error);
    *canary_byte ^= 0x55;
    TEST_ASSERT(Mem_Free(p) == 0, "Clean free after restoring canary");
}

static void test_negative_cases(void) {
    int res = Mem_Init(65536);
    TEST_ASSERT(res == 0, "Mem_Init(65536) must succeed");

    /* 1. 1-byte overflow at sizes 1, 7, 8, 13, 16, 24, 100 */
    static const int overflow_sizes[] = {1, 7, 8, 13, 16, 24, 100};
    for (size_t i = 0; i < sizeof(overflow_sizes) / sizeof(overflow_sizes[0]); i++) {
        run_negative_case_overflow(overflow_sizes[i]);
    }

    /* 2. Size that leaves unsplit slack (isolated 256-byte block) */
    void *guard1 = Mem_Alloc(32);
    void *target = Mem_Alloc(240); /* 240 + 8 = 248, aligned to 256 */
    void *guard2 = Mem_Alloc(32);
    TEST_ASSERT(guard1 && target && guard2, "Guard allocs must succeed");
    TEST_ASSERT(Mem_Free(target) == 0, "Free isolated target block");

    /* 200 + 8 = 208; in 256-byte block, remaining is 48 (< 64), so block cannot split */
    void *slack_p = Mem_Alloc(200);
    TEST_ASSERT(slack_p == target, "Alloc 200 must reuse isolated target block");
    /* Corrupt byte inside unsplit slack padding (beyond canary) */
    char *slack_byte = (char *)slack_p + 200 + 8 + 10;
    *slack_byte ^= 0x55;
    m_error = 0;
    int rc = Mem_Free(slack_p);
    TEST_ASSERT(rc == -1 && m_error == E_PADDING_OVERWRITTEN, "Corrupted slack padding must be caught");
    printf("[NEGATIVE TEST] 1-byte overflow in unsplit slack (size 200/256): CAUGHT (rc = %d, m_error = %d [E_PADDING_OVERWRITTEN])\n",
           rc, m_error);
    *slack_byte ^= 0x55;
    TEST_ASSERT(Mem_Free(slack_p) == 0, "Clean free after restoring slack");
    TEST_ASSERT(Mem_Free(guard1) == 0, "Free guard1");
    TEST_ASSERT(Mem_Free(guard2) == 0, "Free guard2");

    /* 3. Double free */
    void *df_p = Mem_Alloc(64);
    TEST_ASSERT(df_p != NULL, "Alloc 64 for double free");
    TEST_ASSERT(Mem_Free(df_p) == 0, "Initial free must succeed");
    m_error = 0;
    rc = Mem_Free(df_p);
    TEST_ASSERT(rc == -1 && m_error == E_BAD_POINTER, "Double free must fail with E_BAD_POINTER");
    printf("[NEGATIVE TEST] Double free                                    : CAUGHT (rc = %d, m_error = %d [E_BAD_POINTER])\n",
           rc, m_error);

    /* 4. Pointer into header (ptr - 8) */
    void *hdr_p = Mem_Alloc(64);
    TEST_ASSERT(hdr_p != NULL, "Alloc 64 for header check");
    m_error = 0;
    rc = Mem_Free((char *)hdr_p - 8);
    TEST_ASSERT(rc == -1 && m_error == E_BAD_POINTER, "Pointer into header must fail with E_BAD_POINTER");
    printf("[NEGATIVE TEST] Pointer into header (ptr - 8)                  : CAUGHT (rc = %d, m_error = %d [E_BAD_POINTER])\n",
           rc, m_error);
    TEST_ASSERT(Mem_Free(hdr_p) == 0, "Clean free of hdr_p");

    /* 5. Pointer outside arena */
    m_error = 0;
    rc = Mem_Free((void *)0x12345678);
    TEST_ASSERT(rc == -1 && m_error == E_BAD_POINTER, "Pointer outside arena must fail with E_BAD_POINTER");
    printf("[NEGATIVE TEST] Pointer outside arena                          : CAUGHT (rc = %d, m_error = %d [E_BAD_POINTER])\n",
           rc, m_error);

    /* 6. Sizes INT_MAX, 0, -1 */
    m_error = 0;
    void *max_p = Mem_Alloc(INT_MAX);
    TEST_ASSERT(max_p == NULL && (m_error == E_NO_SPACE || m_error == E_BAD_ARGS), "Alloc INT_MAX must fail");
    printf("[NEGATIVE TEST] Allocation size INT_MAX                        : CAUGHT (p = NULL, m_error = %d [%s])\n",
           m_error, m_error == E_NO_SPACE ? "E_NO_SPACE" : "E_BAD_ARGS");

    m_error = 0;
    void *zero_p = Mem_Alloc(0);
    TEST_ASSERT(zero_p == NULL && m_error == E_BAD_ARGS, "Alloc 0 must fail with E_BAD_ARGS");
    printf("[NEGATIVE TEST] Allocation size 0                              : CAUGHT (p = NULL, m_error = %d [E_BAD_ARGS])\n",
           m_error);

    m_error = 0;
    void *neg_p = Mem_Alloc(-1);
    TEST_ASSERT(neg_p == NULL && m_error == E_BAD_ARGS, "Alloc -1 must fail with E_BAD_ARGS");
    printf("[NEGATIVE TEST] Allocation size -1                             : CAUGHT (p = NULL, m_error = %d [E_BAD_ARGS])\n",
           m_error);

    TEST_ASSERT(Mem_Check() == 0, "Mem_Check at end of negative tests must succeed");
}

/* ========================================================================= */
/* Test Runner                                                               */
/* ========================================================================= */
static int run_isolated_test(const char *name, void (*test_func)(void)) {
    printf("[ RUN  ] %s\n", name);
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 0;
    }

    if (pid == 0) {
        test_func();
        exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("[ PASS ] %s\n\n", name);
        return 1;
    } else if (WIFSIGNALED(status)) {
        printf("[ FAIL ] %s (Process crashed with signal %d: %s)\n\n",
               name, WTERMSIG(status), strsignal(WTERMSIG(status)));
        return 0;
    } else {
        printf("[ FAIL ] %s (Process failed with exit code %d)\n\n",
               name, WEXITSTATUS(status));
        return 0;
    }
}

int main(void) {
    printf("================================================================================\n");
    printf("   PROJECT 23: A 'BETTER' MALLOC - COMPREHENSIVE TEST SUITE (SEGREGATED BINS)   \n");
    printf("================================================================================\n\n");

    int passed = 0;
    int total = 0;

    #define EXECUTE_TEST(func) do { \
        total++; \
        if (run_isolated_test(#func, func)) passed++; \
    } while (0)

    EXECUTE_TEST(test_uninitialized_and_bad_args);
    EXECUTE_TEST(test_page_rounding_and_double_init);
    EXECUTE_TEST(test_pointer_alignment_and_integrity);
    EXECUTE_TEST(test_canary_buffer_overflow_detection);
    EXECUTE_TEST(test_block_splitting_and_bins);
    EXECUTE_TEST(test_coalescing_cases);
    EXECUTE_TEST(test_interior_pointers_and_bad_pointers);
    EXECUTE_TEST(test_fragmentation_and_first_fit);
    EXECUTE_TEST(test_mem_check_integrity);
    EXECUTE_TEST(test_randomized_stress);
    EXECUTE_TEST(test_negative_cases);

    printf("--------------------------------------------------------------------------------\n");
    printf("TEST RESULTS: %d / %d Tests Passed (%.1f%% Success Rate)\n",
           passed, total, (double)passed / (double)total * 100.0);
    printf("--------------------------------------------------------------------------------\n\n");

    return (passed == total) ? 0 : 1;
}
