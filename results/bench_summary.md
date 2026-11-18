## Empirical Latency Profile (ns/op: Median ± IQR across >=30 repetitions)

| Workload | S0_I0_T0 (Addr-Order) | S0_I1_T0 (Default LIFO) | S1_I1_T0 (Best-Fit) | S2_I1_T0 (Take-Head) | S2_I1_T1 (Split-64) | system_malloc |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Small Allocs (8-128 B)** | 20.0 ± 0.1 ns | 37.1 ± 0.1 ns | 28.0 ± 0.1 ns | 17.3 ± 12.4 ns | 38.8 ± 0.3 ns | 10.2 ± 0.0 ns | 
| **Mixed (16-2048 B)** | 19.5 ± 0.4 ns | 29.9 ± 0.4 ns | 26.3 ± 0.2 ns | 15.9 ± 0.2 ns | 31.0 ± 0.1 ns | 13.0 ± 0.1 ns | 
| **Large Allocs (>4KB)** | 18.9 ± 1.6 ns | 28.0 ± 0.5 ns | 24.5 ± 0.5 ns | 14.5 ± 0.1 ns | 28.5 ± 0.3 ns | 17.0 ± 0.8 ns | 
| **LIFO Free Order** | 40.5 ± 0.5 ns | 54.2 ± 8.7 ns | 56.1 ± 7.4 ns | 32.9 ± 0.2 ns | 55.9 ± 0.5 ns | 20.2 ± 1.3 ns | 
| **FIFO Free Order** | 37.5 ± 0.1 ns | 53.6 ± 0.8 ns | 49.1 ± 1.6 ns | 32.5 ± 1.2 ns | 55.2 ± 0.3 ns | 19.6 ± 0.1 ns | 
| **Random Free Order** | 119.4 ± 0.3 ns | 63.6 ± 0.7 ns | 60.1 ± 7.4 ns | 45.5 ± 0.2 ns | 69.9 ± 1.1 ns | 25.3 ± 0.2 ns | 
| **Fragmentation Stress** | 388.9 ± 22.4 ns | 44.9 ± 5.0 ns | 42.8 ± 0.2 ns | 29.2 ± 1.0 ns | 45.1 ± 5.0 ns | 16.5 ± 2.7 ns | 
| **Hole Reuse (2 MB Arena)** | 777.8 ± 19.6 ns | 133.2 ± 11.3 ns | 114.0 ± 12.9 ns | 106.5 ± 4.0 ns | 138.1 ± 15.8 ns | 60.9 ± 11.5 ns | 

## Complete Memory Efficiency & Fragmentation Table for ALL 8 Workloads

| Workload | Policy | Utilization | Internal Frag | External Frag | Per-Alloc Overhead |
| :--- | :---: | :---: | :---: | :---: | :---: |
| Small Allocs (8-128 B) | S0_I0_T0 | 0.0% | 0.0% | 0.00% | 64 B |
| Small Allocs (8-128 B) | S0_I1_T0 | 0.0% | 0.0% | 0.00% | 64 B |
| Small Allocs (8-128 B) | S1_I1_T0 | 0.0% | 0.0% | 0.00% | 64 B |
| Small Allocs (8-128 B) | S2_I1_T0 | 0.0% | 0.0% | 0.00% | 64 B |
| Small Allocs (8-128 B) | S2_I1_T1 | 0.0% | 0.0% | 0.00% | 64 B |
| Small Allocs (8-128 B) | system_malloc | N/A (unbounded) | N/A | N/A | ~16 B |
| Mixed (16-2048 B) | S0_I0_T0 | 0.0% | 0.0% | 0.00% | 64 B |
| Mixed (16-2048 B) | S0_I1_T0 | 0.0% | 0.0% | 0.00% | 64 B |
| Mixed (16-2048 B) | S1_I1_T0 | 0.0% | 0.0% | 0.00% | 64 B |
| Mixed (16-2048 B) | S2_I1_T0 | 0.0% | 0.0% | 0.00% | 64 B |
| Mixed (16-2048 B) | S2_I1_T1 | 0.0% | 0.0% | 0.00% | 64 B |
| Mixed (16-2048 B) | system_malloc | N/A (unbounded) | N/A | N/A | ~16 B |
| Large Allocs (>4KB) | S0_I0_T0 | 0.0% | 0.0% | 0.00% | 64 B |
| Large Allocs (>4KB) | S0_I1_T0 | 0.0% | 0.0% | 0.00% | 64 B |
| Large Allocs (>4KB) | S1_I1_T0 | 0.0% | 0.0% | 0.00% | 64 B |
| Large Allocs (>4KB) | S2_I1_T0 | 0.0% | 0.0% | 0.00% | 64 B |
| Large Allocs (>4KB) | S2_I1_T1 | 0.0% | 0.0% | 0.00% | 64 B |
| Large Allocs (>4KB) | system_malloc | N/A (unbounded) | N/A | N/A | ~16 B |
| LIFO Free Order | S0_I0_T0 | 0.5% | 8.9% | 0.00% | 64 B |
| LIFO Free Order | S0_I1_T0 | 0.5% | 8.9% | 0.00% | 64 B |
| LIFO Free Order | S1_I1_T0 | 0.5% | 8.9% | 0.00% | 64 B |
| LIFO Free Order | S2_I1_T0 | 0.5% | 8.9% | 0.00% | 64 B |
| LIFO Free Order | S2_I1_T1 | 0.5% | 8.9% | 0.00% | 64 B |
| LIFO Free Order | system_malloc | N/A (unbounded) | N/A | N/A | ~16 B |
| FIFO Free Order | S0_I0_T0 | 0.5% | 8.9% | 0.00% | 64 B |
| FIFO Free Order | S0_I1_T0 | 0.5% | 8.9% | 0.00% | 64 B |
| FIFO Free Order | S1_I1_T0 | 0.5% | 8.9% | 0.00% | 64 B |
| FIFO Free Order | S2_I1_T0 | 0.5% | 8.9% | 0.00% | 64 B |
| FIFO Free Order | S2_I1_T1 | 0.5% | 8.9% | 0.00% | 64 B |
| FIFO Free Order | system_malloc | N/A (unbounded) | N/A | N/A | ~16 B |
| Random Free Order | S0_I0_T0 | 0.5% | 8.9% | 0.00% | 64 B |
| Random Free Order | S0_I1_T0 | 0.5% | 8.9% | 0.00% | 64 B |
| Random Free Order | S1_I1_T0 | 0.5% | 8.9% | 0.00% | 64 B |
| Random Free Order | S2_I1_T0 | 0.5% | 8.9% | 0.00% | 64 B |
| Random Free Order | S2_I1_T1 | 0.5% | 8.9% | 0.00% | 64 B |
| Random Free Order | system_malloc | N/A (unbounded) | N/A | N/A | ~16 B |
| Fragmentation Stress | S0_I0_T0 | 0.5% | 8.6% | 0.53% | 64 B |
| Fragmentation Stress | S0_I1_T0 | 0.5% | 8.6% | 0.53% | 64 B |
| Fragmentation Stress | S1_I1_T0 | 0.5% | 8.6% | 0.53% | 64 B |
| Fragmentation Stress | S2_I1_T0 | 0.5% | 8.6% | 0.53% | 64 B |
| Fragmentation Stress | S2_I1_T1 | 0.5% | 8.6% | 0.53% | 64 B |
| Fragmentation Stress | system_malloc | N/A (unbounded) | N/A | N/A | ~16 B |
| Hole Reuse (2 MB Arena) | S0_I0_T0 | 69.0% | 4.5% | 99.84% | 64 B |
| Hole Reuse (2 MB Arena) | S0_I1_T0 | 69.0% | 4.4% | 99.84% | 64 B |
| Hole Reuse (2 MB Arena) | S1_I1_T0 | 69.0% | 4.3% | 99.84% | 64 B |
| Hole Reuse (2 MB Arena) | S2_I1_T0 | 69.0% | 4.4% | 99.84% | 64 B |
| Hole Reuse (2 MB Arena) | S2_I1_T1 | 69.0% | 4.4% | 99.84% | 64 B |
| Hole Reuse (2 MB Arena) | system_malloc | N/A (unbounded) | N/A | N/A | ~16 B |
