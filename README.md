# A "Better" Malloc (libmem)

A hardened, high-throughput user-level dynamic memory allocator written in C for macOS Apple Silicon (ARM64) and Linux (x86_64).

## Features

- **Single Arena Mapping**: Exactly one `mmap` call during initialization. Zero kernel allocation calls (`sbrk`, `brk`) or libc `malloc`/`free` calls during runtime.
- **Fixed Overhead Physical Headers**: 48-byte headers (`_Static_assert` validated) utilizing an anonymous union overlay.
- **16-Byte Alignment**: All returned payloads satisfy `(addr & 15) == 0`.
- **Canary & Deterministic Slack Poisoning**: Precision 8-byte trailing canaries placed immediately after user payloads (`0xDEADBEEFCAFEBABE`), with slack bytes poisoned with `0xA5`.
- **Segregated Free-List Bins**: 8 power-of-two bins (`<64 B` to `>=4096 B`) with $O(1)$ LIFO insertion and $O(1)$ bidirectional physical neighbor coalescing.
- **Configurable Policies**: Compile-time switches for search heuristic (First-Fit, Best-Fit, Take-Head), insertion policy (Address-Ordered vs LIFO), and split threshold.
- **Interactive Web Interface**: Live interactive visualization bridge powered by Python ctypes.

## Build and Run

```bash
# Build production library (libmem.so) and test binaries
make all

# Run full test suites
make test

# Run 1,000,000-operation randomized stress test
make stress

# Run UndefinedBehaviorSanitizer (UBSan) and AddressSanitizer (ASan)
make sanitize

# Run benchmark matrix
make matrix

# Launch interactive web UI bridge
python3 server.py
```

## Project Structure

- `mem.h` / `mem.c` - Core memory allocator implementation and public interface
- `test_suite.c` - Comprehensive unit and negative boundary tests
- `stress_test.c` - 1,000,000-operation randomized stress test with 1 MB pressure phase
- `bench_workloads.c` - 8-workload benchmarking harness
- `server.py` - ctypes web server bridge for live visualization
- `index.html` - Interactive web UI
- `report/` - Technical documentation and benchmark figures
- `slides/` - Slide deck outline and presentation
- `results/` - Raw telemetry and benchmark data

## License

MIT License
