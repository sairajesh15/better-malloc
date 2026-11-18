# Project 23: A 'Better' Malloc
## Comprehensive Technical Presentation Slide Deck

---

### Slide 1: Title & Executive Summary
- **Title**: Project 23: Engineering and Empirical Evaluation of a Hardened, Segregated-Fit Dynamic Memory Allocator
- **Subtitle**: Source-Compatible Hardening, $O(1)$ LIFO Insertion, and Microsecond-Scale Determinism
- **Target Platform**: Apple Silicon ARM64 (Apple M2, 16 KB Hardware Page Size) and Linux x86_64
- **Core Result**: Eliminates $O(K^2)$ list traversals with $O(1)$ LIFO insertion, cutting fragmentation latency by $8.7\times$ while detecting trailing overruns, double frees, and corrupted pointers.
- **Presenter**: Systems Research and Architecture Laboratory

> **Speaker Notes**:
> Welcome everyone. Today we are presenting Project 23: "A Better Malloc". Our goal was to take the classic CS537 memory allocator interface and build a hardened, production-grade implementation that operates within a single contiguous mmap arena, provides absolute memory corruption detection at deallocation time, and achieves nanosecond-level throughput that rivals and in some workloads surpasses general-purpose system allocators.

---

### Slide 2: Problem Statement & Non-Negotiable Constraints
- **Strict Source Compatibility**: Public API in `mem.h` (`Mem_Init`, `Mem_Alloc`, `Mem_Free`, `Mem_Dump`, `m_error`, `E_*`) remains untouched.
- **Single `mmap` Invariant**: Exactly ONE system call inside `Mem_Init`; second call must fail with `E_BAD_ARGS`.
- **Zero OS Sub-Allocators**: No `sbrk`, `brk`, or subsequent `mmap` calls; standard `malloc`/`free` are strictly forbidden inside `mem.c`.
- **Rigorous Alignment**: Guaranteed 16-byte alignment (`(addr & 15) == 0`), superseding legacy 8-byte requirements.
- **Claims Discipline**: Every number backed by local empirical data; no tail-latency speculation or unsubstantiated claims.

> **Speaker Notes**:
> The constraints of this project are unforgiving: we have zero ability to request more memory from the operating system once initialized. We cannot rely on libc malloc or sbrk. Everything—every block header, free list, alignment pad, and boundary canary—must live inside a single mmap mapping. Furthermore, our performance numbers are strictly empirical, measured on physical Apple M2 hardware using tightly batched runs.

---

### Slide 3: Physical Block Architecture (The 48-Byte Header)
- **Header Footprint**: Exact 48-byte physical block header, strictly asserted at compile-time via `_Static_assert`.
- **Anonymous Union Design**: Overlays free linkage pointers (`next_free`, `prev_free`) onto allocated metadata (`req_size`, canary backup).
- **Physical Boundary Pointers**: `next_phys` and `prev_phys` form a contiguous chain covering 100% of the mapped arena.
- **Magic Number Guard**: Every header begins with 64-bit guard word `0x0123456789ABCDEF` to verify structural validity.

> **Speaker Notes**:
> At the core of the allocator is our 48-byte block header. Notice the space optimization: while a block is free, it needs pointers to link into free-list bins. But while it is allocated, those pointers are unused. We use an anonymous union to store the exact user requested size and a canary copy in those exact same 16 bytes. This keeps the header at exactly 48 bytes—which is a multiple of 16—ensuring that every returned payload pointer is 16-byte aligned.

---

### Slide 4: Boundary Defense: Precision Canaries & Slack Poisoning
- **The Slack Vulnerability**: Allocating an 80-byte request in a 256-byte unsplit block leaves 176 bytes of unprotected slack.
- **Precision Placement**: 8-byte canary (`0xDEADBEEFCAFEBABE`) placed immediately after `payload + req_size`, NOT at block end.
- **Deterministic Slack Poisoning**: All remaining bytes from canary to block capacity are filled with `0xA5`.
- **Deallocation Verification**: `Mem_Free` validates both the 8-byte canary and every trailing `0xA5` pad byte.
- **Immediate Rejection**: Any corruption returns `-1` and sets `m_error = E_PADDING_OVERWRITTEN`.

> **Speaker Notes**:
> In traditional allocators, placing a canary at the very end of a physical block leaves a dangerous blind spot: if splitting is skipped or if alignment rounds up, a buffer overflow can overwrite up to 63 bytes before reaching the canary. Libmem eliminates this completely by writing the canary immediately after the user's requested payload bytes, and filling all remaining slack up to the block boundary with 0xA5. If a developer writes even a single byte past their buffer, Mem_Free catches it instantly.

---

### Slide 5: Segregated Free Lists & Bi-Directional Coalescing
- **8 Power-of-Two Bins**: Free blocks categorized into ranges: `<64`, `64-127`, `128-255`, `...`, `\ge 4096` bytes.
- **Fast Size-Class Lookup**: Direct bitwise computation routes allocation requests directly to the smallest eligible bin.
- **Immediate $O(1)$ Coalescing**: When a block is freed, left and right physical neighbors are checked and coalesced instantly.
- **Loop Bounding**: Physical walks enforce a step limit `(arena_size / sizeof(block_header_t) + 1)` against corrupted loops.
- **Strict $O(1)$ Free**: Default mode (`MEM_ALLOW_INTERIOR = 0`) rejects non-payload pointers in constant time.

> **Speaker Notes**:
> Searching a single monolithic free list causes allocation times to degrade rapidly. We implemented 8 segregated power-of-two bins. An allocation request jumps directly to its target bin, skipping all smaller blocks. Upon freeing, physical neighbor pointers allow bi-directional coalescing in O(1) time without scanning the heap. To prevent infinite loops caused by heap corruption, physical traversals have a hard upper bound proportional to arena size.

---

### Slide 6: Verification & Correctness Methodology
- **Unit Test Suite**: 11 comprehensive tests covering boundary edge cases, double init, alignment, and fragmentation (100% pass rate).
- **Negative Test Matrix**: Verified single-byte overflows at sizes 1, 7, 8, 13, 16, 24, 100 B and unsplit slack (200 in 256 B block).
- **Exhaustive Heap Audit (`Mem_Check`)**: Verifies physical pointer symmetry, total byte conservation, and coalescing invariants.
- **Sanitizers**: Evaluated under UndefinedBehaviorSanitizer (UBSan) and AddressSanitizer (ASan) with zero findings.
- **ASan Caveat**: Explains why ASan cannot natively protect custom mmap pools without explicit redzone hooks.

> **Speaker Notes**:
> We subjected libmem to exhaustive verification. Beyond standard positive unit tests, our negative test harness validates that single-byte overflows across odd and even sizes, double frees, header pointers, and out-of-arena pointers are properly caught. Mem_Check walks the entire heap verifying that every byte is accounted for and that no two adjacent free blocks ever exist uncoalesced.

---

### Slide 7: Randomized Stress Testing & 1 MB Saturation Pressure
- **1,000,000 Operations**: Seeded PRNG (Seed 1337) generating 500k small-heavy (1-256 B) and 500k mixed (1-8192 B) operations.
- **Deterministic Shadow Table**: Every allocated block filled with `(idx * 0x9E3779B9) ^ seed`, verified in full before deallocation.
- **High-Pressure Phase**: 50,000 operations driving a 1 MB arena to 85-95% live saturation with 23,077 handled `E_NO_SPACE` events.
- **Graceful OOM Handling**: Proves that memory exhaustion causes zero heap corruption or resource leaks.
- **Total Coalescing Audit**: Draining all allocations proves complete recovery into a single free block of 1,048,528 bytes.

> **Speaker Notes**:
> To prove stability under real-world churn, we ran a 1-million operation randomized stress test. Every single active block holds a mathematical bit pattern that is audited before freeing. Following the 1M-op test, we introduced an adversarial pressure phase: forcing a 1 MB arena to 85-95% capacity where thousands of allocations fail due to out-of-space. The allocator handled every failure gracefully and drained cleanly back to a single unified free block.

---

### Slide 8: Empirical Performance Results (Apple M2 Hardware)
- **Small Allocs (16-128 B)**: `libmem` achieves **17.3 - 37.1 ns/op** across policies vs 10.2 ns/op for system malloc.
- **Large Allocs (>4 KB)**: `libmem` achieves **14.5 - 28.0 ns/op**, beating macOS system malloc (**17.0 ns/op**).
- **Stack & Queue Patterns**: LIFO and FIFO frees execute in **32.5 - 54.2 ns/op**.
- **Fragmentation Stress**: Default configuration achieves **44.9 ns/op** vs 388.9 ns/op under legacy address-ordering.
- **Hole Reuse (2 MB Arena)**: Default achieves **133.2 ns/op** vs 777.8 ns/op under address-ordering.

> **Speaker Notes**:
> Looking at the performance numbers: on Apple M2 hardware, libmem achieves allocation and free speeds between 14 and 54 nanoseconds per operation. In large allocations, libmem actually outperforms macOS system malloc (14.5 ns vs 17.0 ns) because our fixed-arena architecture avoids the kernel coordination overhead of unbounded general-purpose allocators.

---

### Slide 9: Root-Cause Diagnosis: $O(K^2)$ Address-Ordering vs $O(1)$ LIFO
- **The Anomaly**: Legacy CS537 address-ordered insertion exhibited catastrophic slowdowns on fragmented workloads.
- **Phase 3a Telemetry**: In a 50,000-op run, Address-Ordered insertion traversed **41,446,778 nodes** (mean: 828.9, max: 8,319 nodes/insert).
- **Algorithmic Complexity**: Inserting $K$ freed blocks into an address-sorted list scales as $\sum_{i=1}^K i \approx \frac{K^2}{2}$.
- **The Fix ($O(1)$ LIFO)**: Inserting freed blocks at the head of their segregated bin drops insertion node visits to **0**.
- **Empirical Impact**: Latency drops from **388.9 ns to 44.9 ns** ($8.7\times$ speedup) on Frag Stress, and from **777.8 ns to 133.2 ns** ($5.8\times$ speedup) on Hole Reuse.

> **Speaker Notes**:
> Here is the most crucial finding of the project. The original CS537 specification recommended address-ordered insertion to facilitate coalescing. However, when we profiled it with hardware counters, we discovered that under fragmentation, inserting freed blocks traverses an average of 829 nodes per free, totaling over 41 million node visits! By switching to O(1) LIFO insertion at the head of each segregated bin, insertion visits drop to zero, delivering an 8.7x speedup.

---

### Slide 10: Hole Reuse & Search Policy Trade-Offs
- **Hole Reuse Workload**: 2 MB arena saturated to ~60% with alternate blocks freed, followed by exact and 2x size reallocations.
- **Best-Fit Search Efficiency**: Visited **4.78 nodes/alloc** with **0.00% external fragmentation** on exact reallocations.
- **First-Fit Search Efficiency**: Visited **9.00 nodes/alloc** with **1.02% external fragmentation**.
- **Take-Head Heuristic**: Fast constant-time lookup (**14.5 - 17.3 ns**) but suffers higher fragmentation under constrained arenas.
- **Policy Selection**: LIFO insertion is selected as default; search heuristic is compile-time configurable based on spatial constraints.

> **Speaker Notes**:
> In our Hole Reuse benchmark, we isolated search heuristics in a tight 2 MB arena. Best-Fit demonstrated significantly higher search precision, visiting only 4.78 nodes per allocation compared to 9.00 nodes for First-Fit, while eliminating external fragmentation on exact-size requests. Take-Head is ultra-fast for unconstrained heaps, but Best-Fit remains superior when memory density is paramount.

---

### Slide 11: Honest Assessment: Limitations & Scope
- **Single-Threaded Only**: Zero concurrency synchronization; thread-safe usage requires external locking.
- **Fixed Arena Bound**: Single initial `mmap` allocation cannot expand dynamically when memory is exhausted.
- **Deferred Free-Time Detection**: Boundary overruns and corruptions trigger at `Mem_Free()`, not during the invalid write itself.
- **Stale Reallocated Pointer Hazard**: Double-freeing a block that was already reallocated to a new caller targets active memory.
- **No `realloc`/`calloc`**: Strictly conforms to the legacy CS537 interface without auxiliary APIs.
- **Interior Pointers Disabled**: `MEM_ALLOW_INTERIOR = 0` by default to preserve $O(1)$ deallocation and avoid stale-pointer hazards.

> **Speaker Notes**:
> To maintain complete engineering honesty, we clearly document what libmem is not. It is single-threaded. It does not dynamically grow the heap. It detects corruptions at free time rather than using hardware page faults, and like almost all allocators without persistent tombstone tables, a double-free on a block that has already been coalesced and reallocated cannot be caught. Interior pointer freeing is disabled by default because linear backwards walks destroy O(1) performance.

---

### Slide 12: Conclusion & Deliverables
- **Unified Security & Performance**: Hardened dynamic allocation with 16-byte alignment, precision canaries, and slack poisoning at 14-54 ns/op.
- **Verified Deliverables**:
  - `libmem.so` (Production) & `libmem_test.so` (Test Harness)
  - Full automated test suites (`test_suite`, `test_suite_addr`, `test_suite_interior`)
  - 1M-operation stress harness (`stress_test`) with 1 MB pressure phase
  - Comprehensive LaTeX academic report (`report/report.pdf`)
  - Reproducible benchmark evidence archive in `results/`
- **Source Availability**: GitHub repository with GitHub Pages live demonstration and Docker deployment.

> **Speaker Notes**:
> In conclusion, Project 23 proves that safety and performance do not have to be a trade-off. We have delivered a hardened allocator that complies with all historic constraints while delivering modern nanosecond throughput. All code, test suites, benchmarks, and LaTeX technical reports are committed and reproducible with a single make command. Thank you, and I am happy to take any questions.
