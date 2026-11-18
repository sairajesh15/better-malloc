#!/usr/bin/env python3
import subprocess
import os
import sys

CONFIGS = [
    {
        "id": "S0_I0_T0",
        "name": "First-Fit / Addr-Order / Split-16 (Baseline)",
        "flags": ["-DMEM_SEARCH_POLICY=0", "-DMEM_INSERT_POLICY=0", "-DMEM_SPLIT_THRESHOLD=0"]
    },
    {
        "id": "S0_I1_T0",
        "name": "First-Fit / LIFO / Split-16",
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
    "frag_stress"
]

WORKLOAD_LABELS = {
    "small_allocs": "Small Allocs (8-128 B)",
    "mixed": "Mixed (16-2048 B)",
    "large_allocs": "Large Allocs (>4KB)",
    "lifo_free": "LIFO Free Order",
    "fifo_free": "FIFO Free Order",
    "random_free": "Random Free Order",
    "frag_stress": "Fragmentation Stress"
}

def main():
    print("Compiling and executing Phase 3b Benchmark Matrix across 6 allocator policies...")
    results = {}
    csv_lines = ["config_id,config_name,workload,median_ns_op,iqr_ns_op,utilization,internal_frag,external_frag"]

    for cfg in CONFIGS:
        cid = cfg["id"]
        bin_name = f"bench_{cid.lower()}"
        cmd_compile = ["gcc", "-O2", "-Wall", "-Wextra"]
        if cid != "system_malloc":
            cmd_compile.extend(["-DMEM_TESTING"])
        cmd_compile.extend(cfg["flags"])
        cmd_compile.extend(["mem.c", "bench_workloads.c", "-o", bin_name])

        subprocess.run(cmd_compile, check=True)
        print(f"  [RUNNING] {cfg['name']} ({cid})...")
        run_res = subprocess.run([f"./{bin_name}", cid], capture_output=True, text=True, check=True)

        results[cid] = {}
        for line in run_res.stdout.strip().split("\n"):
            if line.startswith("RESULT,"):
                parts = line.split(",")
                # RESULT,config_id,workload,median,iqr,util,int_frag,ext_frag
                w_name = parts[2]
                med = float(parts[3])
                iqr = float(parts[4])
                util = float(parts[5])
                inf = float(parts[6])
                exf = float(parts[7])
                results[cid][w_name] = {
                    "median": med,
                    "iqr": iqr,
                    "util": util,
                    "int_frag": inf,
                    "ext_frag": exf
                }
                csv_lines.append(f"{cid},\"{cfg['name']}\",{w_name},{med:.2f},{iqr:.2f},{util:.4f},{inf:.4f},{exf:.4f}")

        if os.path.exists(bin_name):
            os.remove(bin_name)

    # Write CSV
    with open("benchmark_results.csv", "w") as f:
        f.write("\n".join(csv_lines) + "\n")
    print("\nSaved benchmark data to benchmark_results.csv")

    # Generate Markdown Table
    print("\n### Phase 3b Latency & Fragmentation Benchmark Matrix (ns/op: Median ± IQR)\n")
    header = "| Workload | S0_I0_T0 (Baseline) | S0_I1_T0 (LIFO) | S1_I1_T0 (Best-Fit) | S2_I1_T0 (Take-Head) | S2_I1_T1 (Split-64) | system_malloc |"
    sep    = "| :--- | :---: | :---: | :---: | :---: | :---: | :---: |"
    print(header)
    print(sep)

    for w in WORKLOAD_ORDER:
        label = WORKLOAD_LABELS[w]
        row = f"| **{label}** | "
        for cfg in CONFIGS:
            cid = cfg["id"]
            d = results[cid][w]
            row += f"{d['median']:.1f} ± {d['iqr']:.1f} ns | "
        print(row)

    print("\n### Memory Efficiency & Fragmentation Metrics Summary (Peak Active State)\n")
    f_header = "| Workload | Metric | S0_I0_T0 (Baseline) | S0_I1_T0 (LIFO) | S1_I1_T0 (Best-Fit) | S2_I1_T0 (Take-Head) | S2_I1_T1 (Split-64) |"
    f_sep    = "| :--- | :--- | :---: | :---: | :---: | :---: | :---: |"
    print(f_header)
    print(f_sep)

    for w in ["lifo_free", "fifo_free", "frag_stress"]:
        label = WORKLOAD_LABELS[w]
        d_base = results["S0_I0_T0"][w]
        d_lifo = results["S0_I1_T0"][w]
        d_best = results["S1_I1_T0"][w]
        d_head = results["S2_I1_T0"][w]
        d_sp64 = results["S2_I1_T1"][w]

        print(f"| {label} | Internal Frag | {d_base['int_frag']*100:.1f}% | {d_lifo['int_frag']*100:.1f}% | {d_best['int_frag']*100:.1f}% | {d_head['int_frag']*100:.1f}% | {d_sp64['int_frag']*100:.1f}% |")
        print(f"| {label} | External Frag | {d_base['ext_frag']*100:.2f}% | {d_lifo['ext_frag']*100:.2f}% | {d_best['ext_frag']*100:.2f}% | {d_head['ext_frag']*100:.2f}% | {d_sp64['ext_frag']*100:.2f}% |")

if __name__ == "__main__":
    main()
