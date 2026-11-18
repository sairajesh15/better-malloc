#ifndef MEM_H
#define MEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard Error Codes */
#define E_NO_SPACE             1
#define E_CORRUPT_FREESPACE    2
#define E_PADDING_OVERWRITTEN  3
#define E_BAD_ARGS             4
#define E_BAD_POINTER          5

/* 8-Byte Trailing Canary Constant & Padding Byte */
#define CANARY_MAGIC           0xDEADBEEFCAFEBABEU
#define CANARY_PAD_BYTE        0xA5

/* Default Payload Alignment (16-byte aligned, 48 % 16 == 0) */
#ifndef MEM_ALIGN
#define MEM_ALIGN              16
#endif

/* Policy Configuration Defaults */
#ifndef MEM_SEARCH_POLICY
#define MEM_SEARCH_POLICY      0 /* 0 = First-Fit, 1 = Best-Fit, 2 = Take-Head */
#endif

#ifndef MEM_INSERT_POLICY
#define MEM_INSERT_POLICY      1 /* 0 = Address-Ordered, 1 = LIFO (Default) */
#endif

#ifndef MEM_SPLIT_THRESHOLD
#define MEM_SPLIT_THRESHOLD    0 /* 0 = Header+16 B, 1 = Header+64 B */
#endif

/* Number of Segregated Free List Bins */
#define NUM_BINS               8

/* Global status and diagnostic telemetry */
extern int m_error;
extern int m_mmap_syscall_count;

/* Public Memory Allocator API */
int   Mem_Init(int sizeOfRegion);
void *Mem_Alloc(int size);
int   Mem_Free(void *ptr);
void  Mem_Dump(void);
int   Mem_Check(void);

/* Diagnostic Queries & Introspection */
int    Mem_GetMmapCount(void);
int    Mem_GetBinIndex(size_t size);
void  *Mem_GetArenaStart(void);
size_t Mem_GetArenaSize(void);
void   Mem_GetFragmentationMetrics(double *utilization, double *internal_frag, double *external_frag);

#ifdef MEM_TESTING
int   Mem_Reset(void);
void  Mem_SetAllowInterior(int enable);
int   Mem_GetAllowInterior(void);
#endif

#ifdef MEM_STATS
typedef struct {
    uint64_t last_alloc_nodes;
    uint64_t last_insert_nodes;
    uint64_t total_alloc_nodes;
    uint64_t total_alloc_scans;
    uint64_t total_insert_nodes;
    uint64_t total_insert_ops;
    uint64_t total_splits;
    uint64_t total_coalesces;
} mem_stats_summary_t;

void     Mem_Stats_Reset(void);
void     Mem_Stats_Get(mem_stats_summary_t *stats);
uint64_t Mem_Stats_GetLastAllocNodes(void);
uint64_t Mem_Stats_GetLastInsertNodes(void);
uint64_t Mem_Stats_GetTotalSplits(void);
uint64_t Mem_Stats_GetTotalCoalesces(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MEM_H */
