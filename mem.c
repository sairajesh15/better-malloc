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
#define ALIGN8(s) (((size_t)(s) + 7) & ~((size_t)7))

/* Global error status and syscall counter */
int m_error = 0;
int m_mmap_syscall_count = 0;

/*
 * Block header structure tracking payload size, allocation status,
 * contiguous physical neighbors, and doubly-linked free list pointers.
 * Sized to 48 bytes on 64-bit architectures, maintaining strict 8-byte alignment.
 */
typedef struct block_header {
    size_t size;                     /* Usable payload size in bytes (multiple of 8) */
    int is_free;                     /* 1 = free, 0 = allocated */
    uint32_t magic;                  /* Validation magic constant */
    struct block_header *prev_phys;  /* Contiguous physical block before this block */
    struct block_header *next_phys;  /* Contiguous physical block after this block */
    struct block_header *prev_free;  /* Doubly-linked free list: previous free block */
    struct block_header *next_free;  /* Doubly-linked free list: next free block */
} block_header_t;

_Static_assert(sizeof(block_header_t) % 8 == 0, "block_header_t must be 8-byte aligned");

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
    block->is_free = 1;
    int bin = Mem_GetBinIndex(block->size);

    if (free_bins[bin] == NULL) {
        free_bins[bin] = block;
        block->prev_free = NULL;
        block->next_free = NULL;
        return;
    }

    /* Maintain ascending address order inside the bin */
    if (block < free_bins[bin]) {
        block->next_free = free_bins[bin];
        block->prev_free = NULL;
        free_bins[bin]->prev_free = block;
        free_bins[bin] = block;
        return;
    }

    block_header_t *curr = free_bins[bin];
    while (curr->next_free != NULL && curr->next_free < block) {
        curr = curr->next_free;
    }

    block->next_free = curr->next_free;
    block->prev_free = curr;
    if (curr->next_free != NULL) {
        curr->next_free->prev_free = block;
    }
    curr->next_free = block;
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
 * Helper: Resolve a pointer (exact payload start or interior pointer)
 * to its enclosing block header. Returns NULL if invalid or out of bounds.
 */
static block_header_t *find_block_for_ptr(void *ptr) {
    if (!is_initialized || arena_start == NULL || ptr == NULL) {
        return NULL;
    }

    char *p = (char *)ptr;
    if (p < (char *)arena_start || p >= (char *)arena_end) {
        return NULL;
    }

    /* Fast path: Check if ptr is an exact payload start pointer */
    if (((uintptr_t)p % 8 == 0) && (p >= (char *)arena_start + sizeof(block_header_t))) {
        block_header_t *cand = (block_header_t *)(p - sizeof(block_header_t));
        if ((char *)cand >= (char *)arena_start && (char *)cand < (char *)arena_end) {
            if (cand->magic == BLOCK_MAGIC) {
                int valid = 0;
                if (cand == (block_header_t *)arena_start) {
                    if (cand->prev_phys == NULL) {
                        valid = 1;
                    }
                } else {
                    if (cand->prev_phys != NULL && cand->prev_phys->next_phys == cand) {
                        valid = 1;
                    }
                }
                if (valid) {
                    if (cand->next_phys == NULL) {
                        if ((char *)cand + sizeof(block_header_t) + cand->size == (char *)arena_end) {
                            return cand;
                        }
                    } else {
                        if (cand->next_phys->prev_phys == cand &&
                            (char *)cand + sizeof(block_header_t) + cand->size == (char *)cand->next_phys) {
                            return cand;
                        }
                    }
                }
            }
        }
    }

    /* General / Interior pointer lookup: Walk contiguous physical blocks */
    block_header_t *curr = (block_header_t *)arena_start;
    while (curr != NULL && (char *)curr < (char *)arena_end) {
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
 * Enforces strict 8-byte alignment and arms an 8-byte trailing canary.
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

    size_t req_size = ALIGN8(size + 8);

    /* Search starting from the appropriate segregated bin and progress upwards */
    int start_bin = Mem_GetBinIndex(req_size);
    block_header_t *curr = NULL;

    for (int b = start_bin; b < NUM_BINS; b++) {
        curr = free_bins[b];
        while (curr != NULL) {
            if (curr->magic != BLOCK_MAGIC) {
                m_error = E_CORRUPT_FREESPACE;
                return NULL;
            }
            if (curr->is_free && curr->size >= req_size) {
                goto block_found;
            }
            curr = curr->next_free;
        }
    }

    /* Out of space across all bins */
    m_error = E_NO_SPACE;
    return NULL;

block_found:
    remove_from_bin(curr);

    size_t remaining = curr->size - req_size;
    if (remaining >= sizeof(block_header_t) + 16) {
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

        curr->prev_free = NULL;
        curr->next_free = NULL;
        curr->is_free = 0;
    } else {
        curr->is_free = 0;
    }

    /* Arm 8-Byte Trailing Canary at the very end of the allocated payload */
    uint64_t *canary_ptr = (uint64_t *)((char *)curr + sizeof(block_header_t) + curr->size - 8);
    *canary_ptr = CANARY_MAGIC;

    m_error = 0;
    return (void *)((char *)curr + sizeof(block_header_t));
}

/*
 * Mem_Free: Deallocate block (exact payload start or interior pointer).
 * Validates 8-Byte Trailing Canary (0xDEADBEEFCAFEBABE) to catch buffer overruns.
 * Performs immediate physical-neighbor coalescing with adjacent free blocks.
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

    /* Verify 8-Byte Trailing Canary Footer */
    uint64_t *canary_ptr = (uint64_t *)((char *)block + sizeof(block_header_t) + block->size - 8);
    if (*canary_ptr != CANARY_MAGIC) {
        m_error = E_PADDING_OVERWRITTEN;
        return -1;
    }

    /* Clear canary in free block */
    *canary_ptr = 0;

    /* Coalesce with next physical block if free */
    if (block->next_phys != NULL && block->next_phys->is_free) {
        block_header_t *next = block->next_phys;
        remove_from_bin(next);
        block->size += sizeof(block_header_t) + next->size;
        block->next_phys = next->next_phys;
        if (block->next_phys != NULL) {
            block->next_phys->prev_phys = block;
        }
        next->magic = 0;
    }

    /* Coalesce with prev physical block if free */
    if (block->prev_phys != NULL && block->prev_phys->is_free) {
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

    m_error = 0;
    return 0;
}

/*
 * Mem_Dump: Diagnostic utility printing memory map and segregated bin status.
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

    block_header_t *curr = (block_header_t *)arena_start;
    size_t index = 0;
    size_t count_alloc = 0;
    size_t count_free = 0;
    size_t bytes_alloc = 0;
    size_t bytes_free = 0;

    while (curr != NULL && (char *)curr < (char *)arena_end) {
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
    return 0;
}
