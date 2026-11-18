import os
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN

prs = Presentation()
prs.slide_width = Inches(13.333)
prs.slide_height = Inches(7.5)

# Color palette
C_NAVY = RGBColor(26, 37, 47)      # #1a252f
C_BLUE = RGBColor(41, 128, 185)    # #2980b9
C_DARK = RGBColor(44, 62, 80)      # #2c3e50
C_GRAY = RGBColor(127, 140, 141)  # #7f8c8d
C_BG   = RGBColor(248, 249, 250)   # #f8f9fa
C_WHITE= RGBColor(255, 255, 255)
C_RED  = RGBColor(231, 76, 60)

slides_data = [
    {
        "title": "Project 23: A 'Better' Malloc",
        "subtitle": "Hardened, Segregated-Fit Dynamic Memory Allocator on Apple Silicon ARM64",
        "bullets": [
            "Source-compatible implementation of the historic CS537 interface with modern defense-in-depth hardening.",
            "Single contiguous mmap arena: exactly ONE system call inside Mem_Init; zero runtime allocations or sbrk.",
            "Guaranteed 16-byte alignment, 48-byte physical block header, precision canaries, and deterministic slack poisoning.",
            "Eliminates O(K^2) free-list traversal bottlenecks via O(1) LIFO insertion, delivering up to 8.7x speedup.",
            "Exhaustive verification: 1M-op randomized stress harness, 1 MB saturation pressure phase, and Mem_Check() audit."
        ],
        "notes": "Welcome to the presentation for Project 23. Today we present a hardened, high-performance dynamic memory allocator designed for safety-critical and constrained environments."
    },
    {
        "title": "Problem Statement & Hard Constraints",
        "subtitle": "Strict Interface Compatibility and Zero-Runtime-Syscall Discipline",
        "bullets": [
            "Source Compatibility: Public API in mem.h (Mem_Init, Mem_Alloc, Mem_Free, Mem_Dump, m_error, E_*) is strictly preserved.",
            "Single mmap Invariant: Exactly ONE mmap call inside Mem_Init; subsequent calls rejected with E_BAD_ARGS.",
            "No Sub-allocators: No sbrk, brk, or secondary mmap calls. Standard malloc/free are forbidden in mem.c.",
            "Guaranteed 16-Byte Alignment: Enforces (addr & 15) == 0, exceeding legacy 8-byte requirements.",
            "Honest Benchmarking Discipline: Every metric derived from real hardware measurements; no tail-latency speculation."
        ],
        "notes": "The allocator must manage all metadata and payloads within a single pre-reserved arena. No runtime growth is permitted."
    },
    {
        "title": "Physical Block Architecture (48-Byte Header)",
        "subtitle": "Anonymous Union Header Overlay & 16-Byte Boundary Alignment",
        "bullets": [
            "Exact 48-Byte Header: Validated at compile time via _Static_assert(sizeof(block_header_t) == 48).",
            "Anonymous Union Overlay: Reuses next_free / prev_free storage for req_size and canary copy while allocated.",
            "Physical Continuity: next_phys and prev_phys form a continuous double-linked chain across 100% of the arena.",
            "64-Bit Magic Guard: 0x0123456789ABCDEF validates structural integrity before any pointer mutation.",
            "Zero Dynamic Metadata Waste: Allocator metadata footprint is fixed and predictable."
        ],
        "notes": "By overlaying requested size on unused free-list pointers, we preserve a 48-byte header, which is an exact multiple of 16."
    },
    {
        "title": "Precision Canaries & Deterministic Slack Poisoning",
        "subtitle": "Closing the Trailing Overflow Vulnerability in Unsplit Chunks",
        "bullets": [
            "The Slack Hazard: Allocating an 80-byte request in a 256-byte chunk leaves 176 bytes of unprotected slack.",
            "Precision Placement: 8-byte canary (0xDEADBEEFCAFEBABE) written immediately after payload + req_size, NOT at block end.",
            "Deterministic Poisoning: All remaining slack bytes up to block capacity are poisoned with 0xA5.",
            "Deallocation Audit: Mem_Free validates both the canary and every trailing 0xA5 byte.",
            "Immediate Rejection: Corruption returns -1 and sets m_error = E_PADDING_OVERWRITTEN."
        ],
        "notes": "Canaries placed at block boundaries miss small trailing overflows. Our precision placement catches single-byte overflows at any size."
    },
    {
        "title": "Segregated Free Lists & Bi-Directional Coalescing",
        "subtitle": "8 Power-of-Two Bins with O(1) Physical Neighbor Consolidation",
        "bullets": [
            "8 Segregated Bins: Categorized into [<64 B], [64–127], [128–255], [256–511], [512–1023], [1024–2047], [2048–4095], [>=4096 B].",
            "Direct Bin Indexing: Fast bitwise scan routes allocation requests directly to the smallest eligible bin.",
            "Bi-Directional Coalescing: Inspects physical neighbors upon Mem_Free, unlinking and merging free blocks in O(1) time.",
            "Hardened Loop Bound: Traversal steps limited to (arena_size / sizeof(block_header_t) + 1) to prevent infinite loops.",
            "Strict O(1) Free Path: Default mode (MEM_ALLOW_INTERIOR = 0) validates payload pointers in constant time."
        ],
        "notes": "Segregated bins eliminate linear scans across small blocks, while physical links enable instant bi-directional coalescing."
    },
    {
        "title": "Verification & Correctness Methodology",
        "subtitle": "Unit Tests, Negative Boundary Invariants, and Sanitizers",
        "bullets": [
            "Unit Test Suite: 11 tests covering boundary edge cases, double init, alignment, and fragmentation (100% pass rate).",
            "Negative Test Matrix: Single-byte overflows at sizes 1, 7, 8, 13, 16, 24, 100 B and unsplit slack (200 in 256 B) caught.",
            "Exhaustive Mem_Check(): Audits physical pointer symmetry, total byte conservation, and coalescing invariants.",
            "Sanitizers: Evaluated under UBSan (allocator + harness) and ASan (harness) with zero defects.",
            "ASan Limits: Explains why ASan cannot natively protect custom mmap pools without explicit redzone hooks."
        ],
        "notes": "Every negative test case prints a clean pass line. The allocator passed all tests under both UBSan and ASan."
    },
    {
        "title": "1,000,000-Op Stress Test & 1 MB Saturation Phase",
        "subtitle": "Mathematical Pattern Auditing and Graceful Exhaustion Handling",
        "bullets": [
            "1,000,000 Operations: Seed 1337 driving 500k small-heavy (1–256 B) and 500k mixed (1–8192 B) operations.",
            "Shadow Pattern Table: Every payload contains (idx * 0x9E3779B9) ^ seed, audited before every free.",
            "1 MB Pressure Phase: 50,000 ops driving 1 MB arena to 85–95% saturation with 23,077 handled E_NO_SPACE events.",
            "Zero Corruption on OOM: Proves memory exhaustion causes zero memory leaks or metadata corruption.",
            "Single Block Coalesce: Complete drainage recovers 1 free block of 1,048,528 B (arena_size - 48)."
        ],
        "notes": "The 1M-op stress test with 1 MB pressure phase proves absolute robustness under adversarial memory churn."
    },
    {
        "title": "Empirical Latency & Benchmark Results",
        "subtitle": "Batched Execution on Apple M2 ARM64 (Median ns/op [IQR])",
        "bullets": [
            "Small Allocs: libmem achieves 17.3 – 37.1 ns/op across policies vs 10.2 ns/op for system malloc.",
            "Large Allocs (>4 KB): libmem achieves 14.5 – 28.0 ns/op, outperforming macOS system malloc (17.0 ns/op).",
            "Stack & Queue Frees: LIFO and FIFO frees complete in 32.5 – 54.2 ns/op.",
            "Fragmentation Stress: Default policy achieves 44.9 ns/op vs 388.9 ns/op under legacy address-ordering.",
            "Hole Reuse (2 MB Arena): Default achieves 133.2 ns/op vs 777.8 ns/op under address-ordering."
        ],
        "image": "report/figures/latency_comparison.png",
        "notes": "On warm pages, libmem delivers nanosecond-tier latency across all allocation and deallocation patterns."
    },
    {
        "title": "Root-Cause Diagnosis: Address-Ordered vs. LIFO Insertion",
        "subtitle": "Eliminating the O(K^2) Algorithmic Bottleneck Under Fragmentation",
        "bullets": [
            "The Pathology: Address-Ordered insertion traverses the sorted free list on every free operation.",
            "Empirical Telemetry: In a 50,000-op run, Address-Ordered insertion traversed 41,446,778 nodes (mean: 828.9, max: 8,319).",
            "Algorithmic Scale: Inserting K freed blocks into an address-sorted list scales as sum(i=1..K) ≈ K^2 / 2.",
            "O(1) LIFO Insertion: Inserting at the bin head reduces insertion node visits to exactly 0 nodes.",
            "Speedup: Drops fragmentation stress latency from 388.9 ns to 44.9 ns (8.7x) and hole reuse from 777.8 ns to 133.2 ns (5.8x)."
        ],
        "notes": "Hardware counters proved that Address-Ordered insertion scaled quadratically, justifying the default switch to LIFO."
    },
    {
        "title": "Hole Reuse & Search Policy Differentiation",
        "subtitle": "Evaluating First-Fit, Best-Fit, and Take-Head in a 2 MB Arena",
        "bullets": [
            "Constrained Arena: Saturated to ~60% capacity with alternate blocks freed; followed by exact and 2x size reallocations.",
            "Best-Fit Precision: Visited 4.78 nodes/alloc with 0.00% external fragmentation on exact-size reallocations.",
            "First-Fit Performance: Visited 9.00 nodes/alloc with 1.02% external fragmentation.",
            "Take-Head Heuristic: Fastest latency (14.5 – 17.3 ns) but higher fragmentation under severe spatial pressure.",
            "Policy Decision: LIFO insertion is default; search heuristic is compile-time selectable for space vs speed."
        ],
        "image": "report/figures/hole_reuse_analysis.png",
        "notes": "Best-Fit offers optimal spatial packing and lower scan counts, while Take-Head maximizes raw allocation speed."
    },
    {
        "title": "Honest Assessment: Limitations & Scope",
        "subtitle": "Clear Delineation of Threat Model and Architectural Boundaries",
        "bullets": [
            "Single-Threaded: Zero internal mutexes; concurrent execution requires external synchronization.",
            "Fixed Capacity: Single initial mmap cannot expand dynamically when arena capacity is exhausted.",
            "Deferred Detection: Overruns and corruptions are caught at Mem_Free() time, not during active writes.",
            "Reallocated Double-Free Hazard: Double free of an already coalesced and reallocated block targets active memory.",
            "No realloc/calloc: Adheres strictly to the historic CS537 interface without auxiliary APIs.",
            "Interior Pointers Off: MEM_ALLOW_INTERIOR = 0 by default to guarantee O(1) deallocations and avoid stale-pointer hazards."
        ],
        "notes": "In adherence to our strict claims policy, we clearly outline the boundaries and single-threaded design of the allocator."
    },
    {
        "title": "Conclusion & Deliverables",
        "subtitle": "High Throughput Unified with Verifiable Defense-in-Depth",
        "bullets": [
            "Delivered Artifacts: libmem.so, libmem_test.so, test_suite, stress_test, and LaTeX technical report.",
            "Complete Reproduction: Verified via make clean && make test && make stress && make sanitize && make matrix.",
            "Empirical Evidence: All benchmark data preserved in results/bench.csv and results/env.txt.",
            "Web Bridge & Static Deployment: Interactive visualization with fallback to verified offline hardware data.",
            "Repository v1.0: Full git history, comprehensive README, MIT License, and release tag."
        ],
        "notes": "Project 23 proves that rigorous hardening and high performance can be successfully unified. Thank you!"
    }
]

blank_slide_layout = prs.slide_layouts[6]

for slide_idx, sdata in enumerate(slides_data):
    slide = prs.slides.add_slide(blank_slide_layout)
    
    # Background card
    shape = slide.shapes.add_shape(
        1, # MSO_SHAPE.RECTANGLE
        Inches(0.5), Inches(0.5), Inches(12.333), Inches(6.5)
    )
    shape.fill.solid()
    shape.fill.fore_color.rgb = C_WHITE
    shape.line.color.rgb = RGBColor(220, 224, 230)
    shape.line.width = Pt(1)

    # Header Bar
    top_bar = slide.shapes.add_shape(
        1,
        Inches(0.5), Inches(0.5), Inches(12.333), Inches(1.1)
    )
    top_bar.fill.solid()
    top_bar.fill.fore_color.rgb = C_NAVY
    top_bar.line.color.rgb = C_NAVY

    # Title Text
    tx_box = slide.shapes.add_textbox(Inches(0.8), Inches(0.55), Inches(11.7), Inches(0.5))
    tf = tx_box.text_frame
    tf.word_wrap = True
    p = tf.paragraphs[0]
    p.text = sdata["title"]
    p.font.size = Pt(22)
    p.font.bold = True
    p.font.color.rgb = C_WHITE

    # Subtitle Text
    tx_box_sub = slide.shapes.add_textbox(Inches(0.8), Inches(1.1), Inches(11.7), Inches(0.4))
    tf_sub = tx_box_sub.text_frame
    p_sub = tf_sub.paragraphs[0]
    p_sub.text = sdata["subtitle"]
    p_sub.font.size = Pt(13)
    p_sub.font.italic = True
    p_sub.font.color.rgb = RGBColor(189, 195, 199)

    # Bullets
    has_image = "image" in sdata and os.path.exists(sdata["image"])
    bullet_width = Inches(6.5) if has_image else Inches(11.5)

    tx_bullets = slide.shapes.add_textbox(Inches(0.8), Inches(1.8), bullet_width, Inches(4.8))
    tf_bullets = tx_bullets.text_frame
    tf_bullets.word_wrap = True

    for i, bullet in enumerate(sdata["bullets"]):
        p_b = tf_bullets.add_paragraph() if i > 0 else tf_bullets.paragraphs[0]
        p_b.text = "•  " + bullet
        p_b.font.size = Pt(13.5 if not has_image else 12)
        p_b.font.color.rgb = C_DARK
        p_b.space_after = Pt(12)

    # Image if present
    if has_image:
        slide.shapes.add_picture(
            sdata["image"],
            Inches(7.6), Inches(2.0), width=Inches(5.0)
        )

    # Slide Number & Footer
    tx_foot = slide.shapes.add_textbox(Inches(0.8), Inches(6.6), Inches(11.7), Inches(0.3))
    tf_foot = tx_foot.text_frame
    p_f = tf_foot.paragraphs[0]
    p_f.text = f"Project 23: A 'Better' Malloc  |  Slide {slide_idx + 1} of {len(slides_data)}"
    p_f.font.size = Pt(9)
    p_f.font.color.rgb = C_GRAY

    # Speaker Notes
    notes_slide = slide.notes_slide
    text_frame = notes_slide.notes_text_frame
    text_frame.text = sdata["notes"]

prs.save("slides/Project23.pptx")
print("Generated slides/Project23.pptx successfully!")
