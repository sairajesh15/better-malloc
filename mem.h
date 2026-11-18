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
int   Mem_GetMmapCount(void);
int   Mem_GetBinIndex(size_t size);
void *Mem_GetArenaStart(void);
size_t Mem_GetArenaSize(void);

#ifdef MEM_TESTING
int   Mem_Reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MEM_H */
