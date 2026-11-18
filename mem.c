#include "mem.h"
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define BLOCK_MAGIC 0x4D454D59U /* 'MEMY' */

#ifndef MEM_ALIGN
#define MEM_ALIGN 16
#endif

#define ALIGN(s) (((size_t)(s) + ((size_t)MEM_ALIGN - 1)) & ~((size_t)MEM_ALIGN - 1))

/*
 * Compile-time flag: MEM_ALLOW_INTERIOR (default 0)
 * When 0, Mem_Free accepts only exact payload-start pointers (O(1) path).
 * When 1, Mem_Free resolves interior pointers (payload_start <= ptr < payload_end).
 */
#ifndef MEM_ALLOW_INTERIOR
#define MEM_ALLOW_INTERIOR 0
#endif

/*
 * Compile-time Policy Switches:
 * 1. Search Policy (MEM_SEARCH_POLICY):
 *    0 = SEARCH_FIRST_FIT (First-fit scan across all bins)
 *    1 = SEARCH_BEST_FIT  (Best-fit in start bin, then higher bins)
 *    2 = SEARCH_TAKE_HEAD (First-fit in start bin; in higher bins, take head O(1))
 *
 * 2. Insertion Policy (MEM_INSERT_POLICY):
 *    0 = INSERT_ADDR_ORDER (Maintain ascending address order in bin)
 *    1 = INSERT_LIFO       (Insert at head of bin in O(1) time)
 *
 * 3. Split Threshold (MEM_SPLIT_THRESHOLD):
 *    0 = SPLIT_THRESH_16   (header + 16 bytes = 64 bytes)
 *    1 = SPLIT_THRESH_64   (header + 64 bytes = 112 bytes)
 */
#ifndef MEM_SEARCH_POLICY
#define MEM_SEARCH_POLICY 0
#endif

#ifndef MEM_INSERT_POLICY
#define MEM_INSERT_POLICY 0 /* Default: Address-ordered insertion (guarantees lowest-address first-fit reuse) */
#endif

#ifndef MEM_SPLIT_THRESHOLD
#define MEM_SPLIT_THRESHOLD 0
#endif

/* Global error status and syscall counter */
int m_error = 0;
int m_mmap_syscall_count = 0;

#ifdef MEM_STATS
static uint64_t g_stat_last_alloc_nodes = 0;
static uint64_t g_stat_last_insert_nodes = 0;
static uint64_t g_stat_total_alloc_nodes = 0;
static uint64_t g_stat_total_alloc_scans = 0;
static uint64_t g_stat_total_insert_nodes = 0;
static uint64_t g_stat_total_insert_ops = 0;
static uint64_t g_stat_total_splits = 0;
static uint64_t g_stat_total_coalesces = 0;

void Mem_Stats_Reset(void) {
    g_stat_last_alloc_nodes = 0;
    g_stat_last_insert_nodes = 0;
    g_stat_total_alloc_nodes = 0;
    g_stat_total_alloc_scans = 0;
    g_stat_total_insert_nodes = 0;
    g_stat_total_insert_ops = 0;
    g_stat_total_splits = 0;
    g_stat_total_coalesces = 0;
}

void Mem_Stats_Get(mem_stats_summary_t *stats) {
    if (!stats) return;
    stats->last_alloc_nodes = g_stat_last_alloc_nodes;
    stats->last_insert_nodes = g_stat_last_insert_nodes;
    stats->total_alloc_nodes = g_stat_total_alloc_nodes;
    stats->total_alloc_scans = g_stat_total_alloc_scans;
    stats->total_insert_nodes = g_stat_total_insert_nodes;
    stats->total_insert_ops = g_stat_total_insert_ops;
    stats->total_splits = g_stat_total_splits;
    stats->total_coalesces = g_stat_total_coalesces;
}

uint64_t Mem_Stats_GetLastAllocNodes(void) { return g_stat_last_alloc_nodes; }
uint64_t Mem_Stats_GetLastInsertNodes(void) { return g_stat_last_insert_nodes; }
uint64_t Mem_Stats_GetTotalSplits(void) { return g_stat_total_splits; }
uint64_t Mem_Stats_GetTotalCoalesces(void) { return g_stat_total_coalesces; }
#endif

/*
 * Block header structure tracking payload size, allocation status,
 * contiguous physical neighbors, and doubly-linked free list pointers.
 * Sized to 48 bytes on 64-bit architectures, maintaining strict 16-byte alignment.
 *
 * Canary Placement Architecture:
 * While a block is free, prev_free and next_free link it into its segregated bin.
 * While allocated, an anonymous union overlays req_size (the exact user-requested
 * byte count) over prev_free/next_free. This allows Mem_Alloc to place the 8-byte
 * canary immediately after the requested bytes (payload + req_size), and fill
 * any remaining slack bytes up to block->size with CANARY_PAD_BYTE (0xA5).
 * In Mem_Free, both the canary and all padding bytes are validated.
 */
typedef struct block_header {
    size_t size;                     /* Usable block capacity in bytes (multiple of MEM_ALIGN) */
    int is_free;                     /* 1 = free, 0 = allocated */
    uint32_t magic;                  /* Validation magic constant */
    struct block_header *prev_phys;  /* Contiguous physical block before this block */
    struct block_header *next_phys;  /* Contiguous physical block after this block */
    union {
        struct {
            struct block_header *prev_free;  /* Doubly-linked free list: previous free block */
            struct block_header *next_free;  /* Doubly-linked free list: next free block */
        };
        struct {
            size_t req_size;                 /* Exact user-requested allocation size */
            uint64_t _pad_alloc;             /* 8-byte padding to maintain 16-byte union */
        };
    };
} block_header_t;

_Static_assert(sizeof(block_header_t) == 48, "block_header_t must be exactly 48 bytes");
_Static_assert(sizeof(block_header_t) % MEM_ALIGN == 0, "block_header_t must be MEM_ALIGN-aligned");

/* Arena management state */
static void *arena_start = NULL;
static void *arena_end = NULL;
static size_t arena_size = 0;
static int is_initialized = 0;

/* 8 Power-of-Two Segregated Free List Bins */
static block_header_t *free_bins[NUM_BINS] = {NULL};

/* Bin size boundary lookup */
int Mem_GetBinIndex(size_t size) {
    if (size <= 32)   return 0; /* <= 32 B */
    if (size <= 64)   return 1; /* <= 64 B */
    if (size <= 128)  return 2; /* <= 128 B */
    if (size <= 256)  return 3; /* <= 256 B */
    if (size <= 512)  return 4; /* <= 512 B */
    if (size <= 1024) return 5; /* <= 1024 B */
    if (size <= 4096) return 6; /* <= 4096 B */
    return 7;                   /* > 4096 B */
}

int Mem_GetMmapCount(void) {
    return m_mmap_syscall_count;
}

/*
 * Helper: Insert a block into its appropriate segregated bin (address-ordered).
 */
static void insert_into_bin(block_header_t *block) {
#ifdef MEM_STATS
    g_stat_total_insert_ops++;
#endif
    block->is_free = 1;
    int bin = Mem_GetBinIndex(block->size);

#if (MEM_INSERT_POLICY == 1)
    /* LIFO Insertion Policy: O(1) push to head of segregated bin */
    block->next_free = free_bins[bin];
    block->prev_free = NULL;
    if (free_bins[bin] != NULL) {
        free_bins[bin]->prev_free = block;
    }
    free_bins[bin] = block;
#ifdef MEM_STATS
    g_stat_last_insert_nodes = 0;
#endif
    return;

#else
    /* Address-Ordered Insertion Policy */
#ifdef MEM_STATS
    uint64_t ins_nodes = 0;
#endif
    if (free_bins[bin] == NULL) {
        free_bins[bin] = block;
        block->prev_free = NULL;
        block->next_free = NULL;
#ifdef MEM_STATS
        g_stat_last_insert_nodes = 0;
#endif
        return;
    }

    /* Maintain ascending address order inside the bin */
    if (block < free_bins[bin]) {
        block->next_free = free_bins[bin];
        block->prev_free = NULL;
        free_bins[bin]->prev_free = block;
        free_bins[bin] = block;
#ifdef MEM_STATS
        g_stat_last_insert_nodes = 1;
        g_stat_total_insert_nodes += 1;
#endif
        return;
    }

    block_header_t *curr = free_bins[bin];
#ifdef MEM_STATS
    ins_nodes = 1;
#endif
    while (curr->next_free != NULL && curr->next_free < block) {
#ifdef MEM_STATS
        ins_nodes++;
#endif
        curr = curr->next_free;
    }

    block->next_free = curr->next_free;
    block->prev_free = curr;
    if (curr->next_free != NULL) {
        curr->next_free->prev_free = block;
    }
    curr->next_free = block;
#ifdef MEM_STATS
    g_stat_last_insert_nodes = ins_nodes;
    g_stat_total_insert_nodes += ins_nodes;
#endif
#endif
}

/*
 * Helper: Remove a block from its segregated bin.
 */
static void remove_from_bin(block_header_t *block) {
    int bin = Mem_GetBinIndex(block->size);

    if (block->prev_free != NULL) {
        block->prev_free->next_free = block->next_free;
    } else if (free_bins[bin] == block) {
        free_bins[bin] = block->next_free;
    }

    if (block->next_free != NULL) {
        block->next_free->prev_free = block->prev_free;
    }

    block->prev_free = NULL;
    block->next_free = NULL;
}

/*
 * Architectural Note on the Stale-Pointer Hazard:
 * Allowing interior pointer deallocation (MEM_ALLOW_INTERIOR=1) introduces a
 * hazardous programming pattern: when callers pass an interior pointer (e.g.
 * &record->field or buffer + offset), freeing the enclosing parent block silently
 * invalidates all other interior pointers into the same allocation that may still
 * be held by the application. Subsequent calls to free other fields lead to
 * double-free errors, use-after-free vulnerabilities, or undefined memory access.
 * Furthermore, locating the enclosing header requires an O(N) linear walk of
 * physical blocks, destroying the deterministic O(1) deallocation guarantee.
 *
 * By default, MEM_ALLOW_INTERIOR is set to 0. Only exact payload-start pointers
 * are accepted via the O(1) constant-time path; any interior offset immediately
 * fails and sets m_error = E_BAD_POINTER.
 */
static block_header_t *find_block_for_ptr(void *ptr) {
    if (!is_initialized || arena_start == NULL || ptr == NULL) {
        return NULL;
    }

    char *p = (char *)ptr;
    if (p < (char *)arena_start || p >= (char *)arena_end) {
        return NULL;
    }

    /* O(1) Fast path: Check if ptr is an exact payload start pointer */
    if (((uintptr_t)p % MEM_ALIGN == 0) && (p >= (char *)arena_start + sizeof(block_header_t))) {
        block_header_t *cand = (block_header_t *)(p - sizeof(block_header_t));
        if ((char *)cand >= (char *)arena_start && (char *)cand < (char *)arena_end) {
            if (cand->magic == BLOCK_MAGIC) {
                int valid = 0;
                if (cand == (block_header_t *)arena_start) {
                    if (cand->prev_phys == NULL) {
                        valid = 1;
                    }
                } else {
                    if (cand->prev_phys != NULL && cand->prev_phys->next_phys == cand &&
                        cand->prev_phys->magic == BLOCK_MAGIC) {
                        valid = 1;
                    }
                }
                if (valid) {
                    if (cand->next_phys == NULL) {
                        if ((char *)cand + sizeof(block_header_t) + cand->size == (char *)arena_end) {
                            return cand;
                        }
                    } else {
                        if (cand->next_phys->magic == BLOCK_MAGIC &&
                            cand->next_phys->prev_phys == cand &&
                            (char *)cand + sizeof(block_header_t) + cand->size == (char *)cand->next_phys) {
                            return cand;
                        }
                    }
                }
            }
        }
    }

#if MEM_ALLOW_INTERIOR
    /*
     * Interior pointer fallback path: Walk contiguous physical blocks.
     * Hardened with step limit (arena_size / sizeof(block_header_t) + 1) to prevent infinite loops.
     */
    size_t max_steps = arena_size / sizeof(block_header_t) + 1;
    size_t steps = 0;
    block_header_t *curr = (block_header_t *)arena_start;

    while (curr != NULL && (char *)curr < (char *)arena_end && steps++ < max_steps) {
        if (curr->magic != BLOCK_MAGIC) {
            return NULL; /* Metadata corrupted */
        }

        char *payload_start = (char *)curr + sizeof(block_header_t);
        char *payload_end = payload_start + curr->size;

        if (p >= payload_start && p < payload_end) {
            return curr;
        }

        /* Pointer lands inside the block header itself -> invalid pointer */
        if (p >= (char *)curr && p < payload_start) {
            return NULL;
        }

        curr = curr->next_phys;
    }
#endif

    return NULL;
}

/*
 * Mem_Init: Initialize memory allocator with a single mmap call.
 * Rounds sizeOfRegion up to the nearest multiple of getpagesize().
 * Returns 0 on success; -1 and sets m_error = E_BAD_ARGS on failure.
 */
int Mem_Init(int sizeOfRegion) {
    if (is_initialized) {
        m_error = E_BAD_ARGS;
        return -1;
    }

    if (sizeOfRegion <= 0) {
        m_error = E_BAD_ARGS;
        return -1;
    }

    int page_size = getpagesize();
    if (page_size <= 0) {
        page_size = 4096;
    }

    size_t req_size = (size_t)sizeOfRegion;
    size_t remainder = req_size % (size_t)page_size;
    size_t rounded_size = (remainder == 0) ? req_size : (req_size + (size_t)page_size - remainder);

    if (rounded_size < req_size) {
        m_error = E_BAD_ARGS;
        return -1;
    }

    void *ptr = mmap(NULL, rounded_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    m_mmap_syscall_count++;
    if (ptr == MAP_FAILED) {
        m_error = E_BAD_ARGS;
        return -1;
    }

    arena_start = ptr;
    arena_size = rounded_size;
    arena_end = (char *)arena_start + arena_size;
    is_initialized = 1;

    for (int i = 0; i < NUM_BINS; i++) {
        free_bins[i] = NULL;
    }

    /* Initialize initial contiguous free block */
    block_header_t *initial_block = (block_header_t *)arena_start;
    initial_block->size = arena_size - sizeof(block_header_t);
    initial_block->is_free = 1;
    initial_block->magic = BLOCK_MAGIC;
    initial_block->prev_phys = NULL;
    initial_block->next_phys = NULL;
    initial_block->prev_free = NULL;
    initial_block->next_free = NULL;

    insert_into_bin(initial_block);
    m_error = 0;
    return 0;
}

/*
 * Mem_Alloc: Allocate a memory block of at least size bytes.
 * Uses Segregated Free-List Bins with First-Fit and block splitting.
 *
 * Safety & Overflow Guarantees:
 * - Computes required size using unsigned size_t: ALIGN(size + 8).
 * - Rejects size <= 0 or arithmetic overflow with E_BAD_ARGS.
 * - Rejects sizes exceeding usable arena capacity with E_NO_SPACE.
 * - Places 8-byte trailing canary (0xDEADBEEFCAFEBABE) immediately following
 *   the requested bytes (payload + size) via memcpy.
 * - Fills all remaining slack bytes up to block->size with CANARY_PAD_BYTE (0xA5).
 */
void *Mem_Alloc(int size) {
    if (!is_initialized) {
        m_error = E_BAD_ARGS;
        return NULL;
    }

    if (size <= 0) {
        m_error = E_BAD_ARGS;
        return NULL;
    }

    size_t u_size = (size_t)size;

    /* Check for overflow in addition: u_size + 8 */
    if (u_size > SIZE_MAX - sizeof(uint64_t)) {
        m_error = E_BAD_ARGS;
        return NULL;
    }

    size_t raw_needed = u_size + sizeof(uint64_t);
    size_t req_size = ALIGN(raw_needed);

    /* Check for overflow in alignment rounding */
    if (req_size < raw_needed) {
        m_error = E_BAD_ARGS;
        return NULL;
    }

    /* Check if request exceeds total usable arena capacity */
    if (req_size > arena_size - sizeof(block_header_t)) {
        m_error = E_NO_SPACE;
        return NULL;
    }

    /* Search starting from the appropriate segregated bin and progress upwards */
    int start_bin = Mem_GetBinIndex(req_size);
    block_header_t *curr = NULL;
#ifdef MEM_STATS
    uint64_t scan_nodes = 0;
    g_stat_total_alloc_scans++;
#endif

#if (MEM_SEARCH_POLICY == 1)
    /* Best-Fit Search Policy: Find smallest block >= req_size in start bin, then higher bins */
    block_header_t *best_block = NULL;
    size_t best_size = SIZE_MAX;

    curr = free_bins[start_bin];
    while (curr != NULL) {
#ifdef MEM_STATS
        scan_nodes++;
#endif
        if (curr->magic != BLOCK_MAGIC) {
            m_error = E_CORRUPT_FREESPACE;
            return NULL;
        }
        if (curr->is_free && curr->size >= req_size) {
            if (curr->size < best_size) {
                best_size = curr->size;
                best_block = curr;
                if (best_size == req_size) break; /* Exact fit */
            }
        }
        curr = curr->next_free;
    }

    if (best_block != NULL) {
        curr = best_block;
#ifdef MEM_STATS
        g_stat_last_alloc_nodes = scan_nodes;
        g_stat_total_alloc_nodes += scan_nodes;
#endif
        goto block_found;
    }

    for (int b = start_bin + 1; b < NUM_BINS; b++) {
        curr = free_bins[b];
        while (curr != NULL) {
#ifdef MEM_STATS
            scan_nodes++;
#endif
            if (curr->magic != BLOCK_MAGIC) {
                m_error = E_CORRUPT_FREESPACE;
                return NULL;
            }
            if (curr->is_free && curr->size >= req_size) {
                if (curr->size < best_size) {
                    best_size = curr->size;
                    best_block = curr;
                }
            }
            curr = curr->next_free;
        }
        if (best_block != NULL) {
            curr = best_block;
#ifdef MEM_STATS
            g_stat_last_alloc_nodes = scan_nodes;
            g_stat_total_alloc_nodes += scan_nodes;
#endif
            goto block_found;
        }
    }

#elif (MEM_SEARCH_POLICY == 2)
    /* Take-Head in Higher Bins Policy: First-fit in start bin; in higher bins, take head in O(1) */
    curr = free_bins[start_bin];
    while (curr != NULL) {
#ifdef MEM_STATS
        scan_nodes++;
#endif
        if (curr->magic != BLOCK_MAGIC) {
            m_error = E_CORRUPT_FREESPACE;
            return NULL;
        }
        if (curr->is_free && curr->size >= req_size) {
#ifdef MEM_STATS
            g_stat_last_alloc_nodes = scan_nodes;
            g_stat_total_alloc_nodes += scan_nodes;
#endif
            goto block_found;
        }
        curr = curr->next_free;
    }

    for (int b = start_bin + 1; b < NUM_BINS; b++) {
        if (free_bins[b] != NULL) {
            curr = free_bins[b];
#ifdef MEM_STATS
            scan_nodes++;
            g_stat_last_alloc_nodes = scan_nodes;
            g_stat_total_alloc_nodes += scan_nodes;
#endif
            if (curr->magic != BLOCK_MAGIC) {
                m_error = E_CORRUPT_FREESPACE;
                return NULL;
            }
            goto block_found;
        }
    }

#else
    /* Default: First-Fit across all bins */
    for (int b = start_bin; b < NUM_BINS; b++) {
        curr = free_bins[b];
        while (curr != NULL) {
#ifdef MEM_STATS
            scan_nodes++;
#endif
            if (curr->magic != BLOCK_MAGIC) {
                m_error = E_CORRUPT_FREESPACE;
                return NULL;
            }
            if (curr->is_free && curr->size >= req_size) {
#ifdef MEM_STATS
                g_stat_last_alloc_nodes = scan_nodes;
                g_stat_total_alloc_nodes += scan_nodes;
#endif
                goto block_found;
            }
            curr = curr->next_free;
        }
    }
#endif

#ifdef MEM_STATS
    g_stat_last_alloc_nodes = scan_nodes;
    g_stat_total_alloc_nodes += scan_nodes;
#endif
    /* Out of space across all bins */
    m_error = E_NO_SPACE;
    return NULL;

block_found:
    remove_from_bin(curr);

#if (MEM_SPLIT_THRESHOLD == 1)
    /* SPLIT_THRESH_64: require remainder >= header + 64 bytes (112 B) */
    size_t split_threshold = sizeof(block_header_t) + 64;
#else
    /* SPLIT_THRESH_16: require remainder >= header + 16 bytes (64 B) */
    size_t split_threshold = sizeof(block_header_t) + MEM_ALIGN;
#endif

    size_t remaining = curr->size - req_size;
    if (remaining >= split_threshold) {
#ifdef MEM_STATS
        g_stat_total_splits++;
#endif
        char *split_addr = (char *)curr + sizeof(block_header_t) + req_size;
        block_header_t *split_block = (block_header_t *)split_addr;

        split_block->size = remaining - sizeof(block_header_t);
        split_block->is_free = 1;
        split_block->magic = BLOCK_MAGIC;
        split_block->prev_phys = curr;
        split_block->next_phys = curr->next_phys;

        if (split_block->next_phys != NULL) {
            split_block->next_phys->prev_phys = split_block;
        }
        curr->next_phys = split_block;
        curr->size = req_size;

        insert_into_bin(split_block);
    }

    curr->is_free = 0;
    curr->req_size = u_size;
    curr->_pad_alloc = 0;

    /*
     * Canary Placement:
     * Write 8-byte canary (0xDEADBEEFCAFEBABE) immediately after requested bytes.
     * Fill remaining slack up to curr->size with CANARY_PAD_BYTE (0xA5).
     */
    char *payload = (char *)curr + sizeof(block_header_t);
    uint64_t canary = CANARY_MAGIC;
    memcpy(payload + u_size, &canary, sizeof(uint64_t));

    size_t slack_start = u_size + sizeof(uint64_t);
    if (curr->size > slack_start) {
        memset(payload + slack_start, CANARY_PAD_BYTE, curr->size - slack_start);
    }

    m_error = 0;
    return (void *)payload;
}

/*
 * Mem_Free: Deallocate block.
 * Validates:
 * 1. 8-Byte Trailing Canary immediately after requested bytes (payload + req_size).
 * 2. Padding bytes (0xA5) covering all slack up to block->size.
 * If canary or padding is overwritten, returns -1 and sets m_error = E_PADDING_OVERWRITTEN.
 * Performs immediate hardened physical-neighbor coalescing.
 */
int Mem_Free(void *ptr) {
    if (ptr == NULL) {
        return 0;
    }

    if (!is_initialized || arena_start == NULL) {
        m_error = E_BAD_POINTER;
        return -1;
    }

    block_header_t *block = find_block_for_ptr(ptr);
    if (block == NULL || block->is_free) {
        m_error = E_BAD_POINTER;
        return -1;
    }

    char *payload = (char *)block + sizeof(block_header_t);
    size_t req_sz = block->req_size;
    size_t canary_end = req_sz + sizeof(uint64_t);

    if (canary_end > block->size) {
        m_error = E_PADDING_OVERWRITTEN;
        return -1;
    }

    /* Verify 8-Byte Trailing Canary Footer via memcpy (avoids unaligned load fault) */
    uint64_t canary_read = 0;
    memcpy(&canary_read, payload + req_sz, sizeof(uint64_t));
    if (canary_read != CANARY_MAGIC) {
        m_error = E_PADDING_OVERWRITTEN;
        return -1;
    }

    /* Verify padding bytes covering slack between canary and block boundary */
    for (size_t i = canary_end; i < block->size; i++) {
        if ((unsigned char)payload[i] != CANARY_PAD_BYTE) {
            m_error = E_PADDING_OVERWRITTEN;
            return -1;
        }
    }

    /* Clear canary, padding, and requested size metadata before returning to pool */
    memset(payload + req_sz, 0, sizeof(uint64_t));
    block->req_size = 0;
    block->_pad_alloc = 0;
    block->prev_free = NULL;
    block->next_free = NULL;

    /*
     * Hardened Coalescing with next physical block:
     * Validates bounds, magic constant, and bidirectional physical linkage.
     */
    if (block->next_phys != NULL) {
        if ((char *)block->next_phys < (char *)arena_start ||
            (char *)block->next_phys >= (char *)arena_end ||
            block->next_phys->magic != BLOCK_MAGIC ||
            block->next_phys->prev_phys != block) {
            m_error = E_CORRUPT_FREESPACE;
            return -1;
        }

        if (block->next_phys->is_free) {
#ifdef MEM_STATS
            g_stat_total_coalesces++;
#endif
            block_header_t *next = block->next_phys;
            remove_from_bin(next);
            block->size += sizeof(block_header_t) + next->size;
            block->next_phys = next->next_phys;
            if (block->next_phys != NULL) {
                block->next_phys->prev_phys = block;
            }
            next->magic = 0;
        }
    }

    /*
     * Hardened Coalescing with prev physical block:
     * Validates bounds, magic constant, and bidirectional physical linkage.
     */
    if (block->prev_phys != NULL) {
        if ((char *)block->prev_phys < (char *)arena_start ||
            (char *)block->prev_phys >= (char *)arena_end ||
            block->prev_phys->magic != BLOCK_MAGIC ||
            block->prev_phys->next_phys != block) {
            m_error = E_CORRUPT_FREESPACE;
            return -1;
        }

        if (block->prev_phys->is_free) {
#ifdef MEM_STATS
            g_stat_total_coalesces++;
#endif
            block_header_t *prev = block->prev_phys;
            remove_from_bin(prev);
            prev->size += sizeof(block_header_t) + block->size;
            prev->next_phys = block->next_phys;
            if (prev->next_phys != NULL) {
                prev->next_phys->prev_phys = prev;
            }
            block->magic = 0;
            insert_into_bin(prev);
        } else {
            insert_into_bin(block);
        }
    } else {
        insert_into_bin(block);
    }

    m_error = 0;
    return 0;
}

/*
 * Mem_Dump: Diagnostic utility printing memory map and segregated bin status.
 * Hardened with step limit to prevent infinite loops if next_phys is corrupted.
 */
void Mem_Dump(void) {
    if (!is_initialized || arena_start == NULL) {
        printf("--- Mem_Dump: Allocator uninitialized ---\n");
        return;
    }

    printf("\n================================ MEMORY ARENA DUMP ================================\n");
    printf("Arena Range : %p - %p (%zu bytes, Page size: %d, mmap count: %d)\n",
           arena_start, arena_end, arena_size, getpagesize(), m_mmap_syscall_count);
    printf("-----------------------------------------------------------------------------------\n");
    printf("%-6s | %-18s | %-18s | %-12s | %-10s\n",
           "Index", "Header Address", "Payload Address", "Size (bytes)", "Status");
    printf("-----------------------------------------------------------------------------------\n");

    size_t max_steps = arena_size / sizeof(block_header_t) + 1;
    size_t steps = 0;
    block_header_t *curr = (block_header_t *)arena_start;
    size_t index = 0;
    size_t count_alloc = 0;
    size_t count_free = 0;
    size_t bytes_alloc = 0;
    size_t bytes_free = 0;

    while (curr != NULL && (char *)curr < (char *)arena_end && steps++ < max_steps) {
        if (curr->magic != BLOCK_MAGIC) {
            printf("[CORRUPT BLOCK METADATA DETECTED at address %p! Magic: 0x%08X]\n",
                   (void *)curr, curr->magic);
            break;
        }

        void *payload = (void *)((char *)curr + sizeof(block_header_t));
        const char *status_str = curr->is_free ? "FREE" : "ALLOCATED";

        printf("%-6zu | %-18p | %-18p | %-12zu | %-10s\n",
               index, (void *)curr, payload, curr->size, status_str);

        if (curr->is_free) {
            count_free++;
            bytes_free += curr->size;
        } else {
            count_alloc++;
            bytes_alloc += curr->size;
        }

        index++;
        curr = curr->next_phys;
    }

    printf("-----------------------------------------------------------------------------------\n");
    printf("Summary: %zu total blocks (%zu allocated [%zu B payload], %zu free [%zu B payload])\n",
           index, count_alloc, bytes_alloc, count_free, bytes_free);
    printf("Segregated Free Bins (8 Power-of-Two Size Classes):\n");
    const char *bin_names[NUM_BINS] = {
        "<=32B", "<=64B", "<=128B", "<=256B", "<=512B", "<=1024B", "<=4096B", ">4096B"
    };
    for (int b = 0; b < NUM_BINS; b++) {
        printf("  Bin %d (%-8s): ", b, bin_names[b]);
        block_header_t *f = free_bins[b];
        if (!f) {
            printf("empty\n");
        } else {
            while (f != NULL) {
                printf("[%p: %zu B] -> ", (void *)f, f->size);
                f = f->next_free;
            }
            printf("NULL\n");
        }
    }
    printf("===================================================================================\n\n");
}

/*
 * Introspection & Lifecycle management functions
 */
void *Mem_GetArenaStart(void) {
    return arena_start;
}

size_t Mem_GetArenaSize(void) {
    return arena_size;
}

void Mem_GetFragmentationMetrics(double *utilization, double *internal_frag, double *external_frag) {
    if (!is_initialized || arena_start == NULL || arena_size == 0) {
        if (utilization) *utilization = 0.0;
        if (internal_frag) *internal_frag = 0.0;
        if (external_frag) *external_frag = 0.0;
        return;
    }

    size_t active_req_bytes = 0;
    size_t active_block_bytes = 0;
    size_t total_free_bytes = 0;
    size_t largest_free_block = 0;

    size_t max_steps = arena_size / sizeof(block_header_t) + 1;
    size_t steps = 0;
    block_header_t *curr = (block_header_t *)arena_start;

    while (curr != NULL && (char *)curr < (char *)arena_end && steps++ < max_steps) {
        if (curr->magic != BLOCK_MAGIC) break;

        if (curr->is_free) {
            total_free_bytes += curr->size;
            if (curr->size > largest_free_block) {
                largest_free_block = curr->size;
            }
        } else {
            active_req_bytes += curr->req_size;
            active_block_bytes += curr->size;
        }
        curr = curr->next_phys;
    }

    if (utilization) {
        *utilization = (double)active_req_bytes / (double)arena_size;
    }
    if (internal_frag) {
        *internal_frag = (active_block_bytes > 0)
            ? (double)(active_block_bytes - active_req_bytes) / (double)active_block_bytes
            : 0.0;
    }
    if (external_frag) {
        *external_frag = (total_free_bytes > 0)
            ? (1.0 - ((double)largest_free_block / (double)total_free_bytes))
            : 0.0;
    }
}

/*
 * Mem_Check: Exhaustive heap sanity checker.
 * Validates:
 * 1. Allocator initialization invariants (arena bounds, page multiples).
 * 2. Physical block chain consistency:
 *    - Magic tag 0x4D454D59 on all blocks.
 *    - Strict alignment of all sizes (size % MEM_ALIGN == 0).
 *    - Contiguous physical neighbor symmetry:
 *      (curr->next_phys->prev_phys == curr, curr->prev_phys->next_phys == curr).
 *    - No adjacent free blocks (coalescing invariant).
 *    - Physical block size accounting: sum(sizeof(block_header_t) + size) == arena_size.
 * 3. Allocated block integrity:
 *    - Canary constant (0xDEADBEEFCAFEBABE) immediately following payload + req_size.
 *    - Padding bytes (0xA5) covering slack up to block->size.
 * 4. Free list & Segregated bin invariants:
 *    - Every free block is in the correct bin: Mem_GetBinIndex(size).
 *    - Doubly-linked free list symmetry (next_free->prev_free == curr).
 *    - Strict address ordering inside bins.
 *    - Free block count and free payload byte sum across bins exactly equals
 *      free blocks found during the physical walk.
 * Returns 0 if all invariants hold; returns -1 and sets m_error on corruption.
 */
int Mem_Check(void) {
    if (!is_initialized || arena_start == NULL || arena_size == 0) {
        m_error = E_BAD_ARGS;
        return -1;
    }

    if ((uintptr_t)arena_start % (uintptr_t)getpagesize() != 0) {
        m_error = E_CORRUPT_FREESPACE;
        return -1;
    }

    size_t max_steps = arena_size / sizeof(block_header_t) + 1;
    size_t steps = 0;
    block_header_t *curr = (block_header_t *)arena_start;
    size_t total_covered_bytes = 0;
    size_t phys_free_blocks = 0;
    size_t phys_free_bytes = 0;
    block_header_t *prev = NULL;

    while (curr != NULL && steps++ < max_steps) {
        /* Check pointer bounds */
        if ((char *)curr < (char *)arena_start || (char *)curr >= (char *)arena_end) {
            m_error = E_CORRUPT_FREESPACE;
            return -1;
        }

        /* Check block magic tag */
        if (curr->magic != BLOCK_MAGIC) {
            m_error = E_CORRUPT_FREESPACE;
            return -1;
        }

        /* Check payload alignment */
        if (curr->size % MEM_ALIGN != 0 || curr->size < MEM_ALIGN) {
            m_error = E_CORRUPT_FREESPACE;
            return -1;
        }

        /* Verify physical neighbor link symmetry */
        if (curr == (block_header_t *)arena_start) {
            if (curr->prev_phys != NULL) {
                m_error = E_CORRUPT_FREESPACE;
                return -1;
            }
        } else {
            if (curr->prev_phys != prev || prev->next_phys != curr) {
                m_error = E_CORRUPT_FREESPACE;
                return -1;
            }
            if ((char *)prev + sizeof(block_header_t) + prev->size != (char *)curr) {
                m_error = E_CORRUPT_FREESPACE;
                return -1;
            }
        }

        /* Coalescing invariant: Two consecutive free blocks are strictly forbidden */
        if (curr->is_free && prev != NULL && prev->is_free) {
            m_error = E_CORRUPT_FREESPACE;
            return -1;
        }

        /* Check payload & canary integrity for allocated blocks */
        if (!curr->is_free) {
            if (curr->req_size == 0 || curr->req_size + sizeof(uint64_t) > curr->size) {
                m_error = E_PADDING_OVERWRITTEN;
                return -1;
            }

            char *payload = (char *)curr + sizeof(block_header_t);
            uint64_t canary_val = 0;
            memcpy(&canary_val, payload + curr->req_size, sizeof(uint64_t));
            if (canary_val != CANARY_MAGIC) {
                m_error = E_PADDING_OVERWRITTEN;
                return -1;
            }

            size_t slack_start = curr->req_size + sizeof(uint64_t);
            for (size_t i = slack_start; i < curr->size; i++) {
                if ((unsigned char)payload[i] != CANARY_PAD_BYTE) {
                    m_error = E_PADDING_OVERWRITTEN;
                    return -1;
                }
            }
        } else {
            phys_free_blocks++;
            phys_free_bytes += curr->size;
        }

        total_covered_bytes += sizeof(block_header_t) + curr->size;
        prev = curr;
        curr = curr->next_phys;
    }

    if (steps >= max_steps) {
        /* Loop in physical chain */
        m_error = E_CORRUPT_FREESPACE;
        return -1;
    }

    /* Entire arena must be accounted for without gaps or overflows */
    if (total_covered_bytes != arena_size || (char *)prev + sizeof(block_header_t) + prev->size != (char *)arena_end) {
        m_error = E_CORRUPT_FREESPACE;
        return -1;
    }

    /* Free list & Segregated bins validation */
    size_t bin_free_blocks = 0;
    size_t bin_free_bytes = 0;

    for (int b = 0; b < NUM_BINS; b++) {
        block_header_t *f = free_bins[b];
        block_header_t *prev_f = NULL;
        size_t bin_steps = 0;

        while (f != NULL && bin_steps++ < max_steps) {
            if (f->magic != BLOCK_MAGIC || !f->is_free) {
                m_error = E_CORRUPT_FREESPACE;
                return -1;
            }

            if (Mem_GetBinIndex(f->size) != b) {
                m_error = E_CORRUPT_FREESPACE;
                return -1;
            }

            /* Doubly-linked free list symmetry */
            if (prev_f == NULL) {
                if (f->prev_free != NULL) {
                    m_error = E_CORRUPT_FREESPACE;
                    return -1;
                }
            } else {
                if (f->prev_free != prev_f || prev_f->next_free != f) {
                    m_error = E_CORRUPT_FREESPACE;
                    return -1;
                }
#if (MEM_INSERT_POLICY == 0)
                /* Address ordering check (only applicable in address-ordered bins) */
                if (f <= prev_f) {
                    m_error = E_CORRUPT_FREESPACE;
                    return -1;
                }
#endif
            }

            bin_free_blocks++;
            bin_free_bytes += f->size;
            prev_f = f;
            f = f->next_free;
        }

        if (bin_steps >= max_steps) {
            m_error = E_CORRUPT_FREESPACE;
            return -1;
        }
    }

    /* Bins must contain the exact same set of free blocks as the physical walk */
    if (bin_free_blocks != phys_free_blocks || bin_free_bytes != phys_free_bytes) {
        m_error = E_CORRUPT_FREESPACE;
        return -1;
    }

    return 0;
}

#ifdef MEM_TESTING
/*
 * Mem_Reset: Unmaps arena and resets allocator state.
 * Only compiled into test binaries and dev bridge under MEM_TESTING.
 * Excluded from the shipped production library.
 */
int Mem_Reset(void) {
    if (is_initialized && arena_start != NULL && arena_size > 0) {
        munmap(arena_start, arena_size);
    }
    arena_start = NULL;
    arena_end = NULL;
    arena_size = 0;
    is_initialized = 0;
    m_mmap_syscall_count = 0;
    m_error = 0;
    for (int i = 0; i < NUM_BINS; i++) {
        free_bins[i] = NULL;
    }
#ifdef MEM_STATS
    Mem_Stats_Reset();
#endif
    return 0;
}
#endif
