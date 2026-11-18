#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

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

    printf("--------------------------------------------------------------------------------\n");
    printf("TEST RESULTS: %d / %d Tests Passed (%.1f%% Success Rate)\n",
           passed, total, (double)passed / (double)total * 100.0);
    printf("--------------------------------------------------------------------------------\n\n");

    return (passed == total) ? 0 : 1;
}
