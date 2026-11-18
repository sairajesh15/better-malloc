#!/usr/bin/env python3
import subprocess
import os
import sys

CONFIGS = [
    {
        "id": "S0_I0_T0",
        "name": "First-Fit / Addr-Order / Split-16 (CS537)",
        "flags": ["-DMEM_SEARCH_POLICY=0", "-DMEM_INSERT_POLICY=0", "-DMEM_SPLIT_THRESHOLD=0"]
    },
    {
        "id": "S0_I1_T0",
        "name": "First-Fit / LIFO / Split-16 (Default)",
        "flags": ["-DMEM_SEARCH_POLICY=0", "-DMEM_INSERT_POLICY=1", "-DMEM_SPLIT_THRESHOLD=0"]
    },
    {
        "id": "S1_I1_T0",
        "name": "Best-Fit / LIFO / Split-16",
        "flags": ["-DMEM_SEARCH_POLICY=1", "-DMEM_INSERT_POLICY=1", "-DMEM_SPLIT_THRESHOLD=0"]
    },
    {
        "id": "S2_I1_T0",
        "name": "Take-Head / LIFO / Split-16",
        "flags": ["-DMEM_SEARCH_POLICY=2", "-DMEM_INSERT_POLICY=1", "-DMEM_SPLIT_THRESHOLD=0"]
    },
    {
        "id": "S2_I1_T1",
        "name": "Take-Head / LIFO / Split-64",
        "flags": ["-DMEM_SEARCH_POLICY=2", "-DMEM_INSERT_POLICY=1", "-DMEM_SPLIT_THRESHOLD=1"]
    },
    {
        "id": "system_malloc",
        "name": "macOS libsystem_malloc (Thread-safe, Unbounded)",
        "flags": ["-DBENCH_SYSTEM"]
    }
]

WORKLOAD_ORDER = [
    "small_allocs",
    "mixed",
    "large_allocs",
    "lifo_free",
    "fifo_free",
    "random_free",
    "frag_stress",
    "hole_reuse"
]

WORKLOAD_LABELS = {
    "small_allocs": "Small Allocs (8-128 B)",
    "mixed": "Mixed (16-2048 B)",
    "large_allocs": "Large Allocs (>4KB)",
    "lifo_free": "LIFO Free Order",
    "fifo_free": "FIFO Free Order",
    "random_free": "Random Free Order",
    "frag_stress": "Fragmentation Stress",
    "hole_reuse": "Hole Reuse (2 MB Arena)"
}

def main():
    os.makedirs("results", exist_ok=True)
    print("Compiling and executing Comprehensive Benchmark Matrix across 6 allocator policies...")
    results = {}
    csv_header = "config_id,config_name,workload,median_ns_op,iqr_ns_op,utilization,internal_frag,external_frag,overhead_per_alloc,nodes_mean,nodes_p99,nodes_max,failures"
    csv_lines = [csv_header]

    for cfg in CONFIGS:
        cid = cfg["id"]
        bin_name = f"bench_{cid.lower()}"
        cmd_compile = ["gcc", "-O2", "-Wall", "-Wextra"]
        if cid != "system_malloc":
            cmd_compile.extend(["-DMEM_TESTING", "-DMEM_STATS"])
        cmd_compile.extend(cfg["flags"])
        cmd_compile.extend(["mem.c", "bench_workloads.c", "-o", bin_name])

        subprocess.run(cmd_compile, check=True)
        print(f"  [RUNNING] {cfg['name']} ({cid})...")
        run_res = subprocess.run([f"./{bin_name}", cid], capture_output=True, text=True, check=True)

        results[cid] = {}
        for line in run_res.stdout.strip().split("\n"):
            if line.startswith("RESULT,"):
                parts = line.split(",")
                w_name = parts[2]
                med = float(parts[3])
                iqr = float(parts[4])
                util = float(parts[5])
                inf = float(parts[6])
                exf = float(parts[7])
                ovhd = float(parts[8])
                n_mean = float(parts[9])
                n_p99 = float(parts[10])
                n_max = float(parts[11])
                fails = int(parts[12])

                results[cid][w_name] = {
                    "median": med,
                    "iqr": iqr,
                    "util": util,
                    "int_frag": inf,
                    "ext_frag": exf,
                    "overhead": ovhd,
                    "nodes_mean": n_mean,
                    "nodes_p99": n_p99,
                    "nodes_max": n_max,
                    "failures": fails
                }
                csv_lines.append(f"{cid},\"{cfg['name']}\",{w_name},{med:.2f},{iqr:.2f},{util:.4f},{inf:.4f},{exf:.4f},{ovhd:.1f},{n_mean:.2f},{n_p99:.2f},{n_max:.2f},{fails}")

        if os.path.exists(bin_name):
            os.remove(bin_name)

    # Save to both benchmark_results.csv and results/bench.csv
    csv_content = "\n".join(csv_lines) + "\n"
    with open("benchmark_results.csv", "w") as f:
        f.write(csv_content)
    with open("results/bench.csv", "w") as f:
        f.write(csv_content)
    print("\nSaved benchmark data to benchmark_results.csv and results/bench.csv")

    summary_md = []
    summary_md.append("## Empirical Latency Profile (ns/op: Median ± IQR across >=30 repetitions)\n")
    summary_md.append("| Workload | S0_I0_T0 (Addr-Order) | S0_I1_T0 (Default LIFO) | S1_I1_T0 (Best-Fit) | S2_I1_T0 (Take-Head) | S2_I1_T1 (Split-64) | system_malloc |")
    summary_md.append("| :--- | :---: | :---: | :---: | :---: | :---: | :---: |")

    for w in WORKLOAD_ORDER:
        label = WORKLOAD_LABELS[w]
        row = f"| **{label}** | "
        for cfg in CONFIGS:
            cid = cfg["id"]
            d = results[cid][w]
            row += f"{d['median']:.1f} ± {d['iqr']:.1f} ns | "
        summary_md.append(row)

    summary_md.append("\n## Complete Memory Efficiency & Fragmentation Table for ALL 8 Workloads\n")
    summary_md.append("| Workload | Policy | Utilization | Internal Frag | External Frag | Per-Alloc Overhead |")
    summary_md.append("| :--- | :---: | :---: | :---: | :---: | :---: |")

    for w in WORKLOAD_ORDER:
        label = WORKLOAD_LABELS[w]
        for cfg in CONFIGS:
            cid = cfg["id"]
            d = results[cid][w]
            if cid == "system_malloc":
                summary_md.append(f"| {label} | system_malloc | N/A (unbounded) | N/A | N/A | ~16 B |")
            else:
                summary_md.append(f"| {label} | {cid} | {d['util']*100:.1f}% | {d['int_frag']*100:.1f}% | {d['ext_frag']*100:.2f}% | {d['overhead']:.0f} B |")

    md_text = "\n".join(summary_md) + "\n"
    with open("results/bench_summary.md", "w") as f:
        f.write(md_text)

    print("\n" + md_text)

if __name__ == "__main__":
    main()
