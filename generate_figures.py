import os
import csv
import matplotlib
import matplotlib.pyplot as plt
import numpy as np

# Use non-interactive backend and writable cache
os.environ['MPLCONFIGDIR'] = '/tmp/matplotlib_cache'
matplotlib.use('Agg')

csv_file = 'results/bench.csv'
data = []
with open(csv_file, 'r') as f:
    reader = csv.DictReader(f)
    for row in reader:
        data.append(row)

# Workload labels
workload_order = [
    'small_allocs', 'mixed', 'large_allocs', 
    'lifo_free', 'fifo_free', 'random_free', 
    'frag_stress', 'hole_reuse'
]

workload_names = {
    'small_allocs': 'Small Allocs\n(16..128 B)',
    'mixed': 'Mixed\n(16..2048 B)',
    'large_allocs': 'Large Allocs\n(> 4 KB)',
    'lifo_free': 'LIFO Free\n(Stack pattern)',
    'fifo_free': 'FIFO Free\n(Queue pattern)',
    'random_free': 'Random Free\n(Permuted)',
    'frag_stress': 'Frag Stress\n(Alternating)',
    'hole_reuse': 'Hole Reuse\n(2 MB Arena)'
}

# Configs to plot
configs = [
    ('S0_I0_T0', 'CS537 (Addr-Order)', '#e74c3c'),
    ('S0_I1_T0', 'Default (LIFO)', '#2980b9'),
    ('S1_I1_T0', 'Best-Fit (LIFO)', '#27ae60'),
    ('S2_I1_T0', 'Take-Head (LIFO)', '#8e44ad'),
    ('system_malloc', 'System Malloc', '#7f8c8d')
]

# -------------------------------------------------------------
# FIGURE 1: Latency Comparison across Workloads (ns/op, log scale)
# -------------------------------------------------------------
fig, ax = plt.subplots(figsize=(12, 6))

x = np.arange(len(workload_order))
width = 0.16

for i, (cid, label, color) in enumerate(configs):
    medians = []
    iqrs = []
    for wl in workload_order:
        match = [r for r in data if r['config_id'] == cid and r['workload'] == wl]
        if match:
            medians.append(float(match[0]['median_ns_op']))
            iqrs.append(float(match[0]['iqr_ns_op']))
        else:
            medians.append(0.0)
            iqrs.append(0.0)
    
    pos = x + (i - len(configs)/2 + 0.5) * width
    ax.bar(pos, medians, width, yerr=iqrs, capsize=2, label=label, color=color, alpha=0.9, edgecolor='black', linewidth=0.5)

ax.set_ylabel('Execution Time per Operation (ns/op, log scale)', fontsize=11, fontweight='bold')
ax.set_title('Allocation and Free Latency Across Workload Matrix (Apple M2, Clang -O2)', fontsize=12, fontweight='bold', pad=15)
ax.set_xticks(x)
ax.set_xticklabels([workload_names[wl] for wl in workload_order], fontsize=9)
ax.set_yscale('log')
ax.grid(axis='y', linestyle='--', alpha=0.5)
ax.legend(frameon=True, fontsize=9, loc='upper left')

# Annotate specific notable points
# Note the Address-Ordered frag_stress and hole_reuse spikes
ax.annotate('O(K) insertion walk\n(777.8 ns)', xy=(7 - 2*width, 777.75), xytext=(6.5, 950),
            arrowprops=dict(facecolor='black', shrink=0.05, width=0.5, headwidth=4),
            fontsize=8, fontweight='bold', color='#c0392b')

plt.tight_layout()
plt.savefig('report/figures/latency_comparison.pdf', dpi=300)
plt.savefig('report/figures/latency_comparison.png', dpi=300)
plt.close()
print("Generated latency_comparison.pdf and .png")

# -------------------------------------------------------------
# FIGURE 2: External Fragmentation Comparison
# -------------------------------------------------------------
fig, ax = plt.subplots(figsize=(10, 5))

# We look at frag_stress and hole_reuse where external frag occurs
frag_workloads = ['frag_stress', 'hole_reuse']
frag_configs = [
    ('S0_I0_T0', 'First-Fit Addr-Order', '#e74c3c'),
    ('S0_I1_T0', 'First-Fit LIFO', '#2980b9'),
    ('S1_I1_T0', 'Best-Fit LIFO', '#27ae60'),
    ('S2_I1_T0', 'Take-Head LIFO', '#8e44ad'),
    ('S2_I1_T1', 'Take-Head Split-64', '#d35400')
]

x = np.arange(len(frag_workloads))
width = 0.15

for i, (cid, label, color) in enumerate(frag_configs):
    vals = []
    for wl in frag_workloads:
        match = [r for r in data if r['config_id'] == cid and r['workload'] == wl]
        if match:
            vals.append(float(match[0]['external_frag']) * 100.0) # as percentage
        else:
            vals.append(0.0)
    pos = x + (i - len(frag_configs)/2 + 0.5) * width
    ax.bar(pos, vals, width, label=label, color=color, alpha=0.85, edgecolor='black', linewidth=0.5)

ax.set_ylabel('External Fragmentation (%) [1 - largest/total]', fontsize=11, fontweight='bold')
ax.set_title('External Fragmentation Under Adversarial Fragmentation & Hole Reuse', fontsize=12, fontweight='bold', pad=15)
ax.set_xticks(x)
ax.set_xticklabels(['Fragmentation Stress\n(Alternating Free in 64 MB)', 'Hole Reuse Phase 2\n(2x Allocation in 2 MB)'], fontsize=10)
ax.grid(axis='y', linestyle='--', alpha=0.5)
ax.legend(frameon=True, fontsize=9, loc='upper left')

plt.tight_layout()
plt.savefig('report/figures/external_frag.pdf', dpi=300)
plt.savefig('report/figures/external_frag.png', dpi=300)
plt.close()
print("Generated external_frag.pdf and .png")

# -------------------------------------------------------------
# FIGURE 3: Hole Reuse Nodes Scanned & Nodes Visited Breakdown
# -------------------------------------------------------------
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.5))

# Subplot 1: Median Latency in Hole Reuse
labels = ['Addr-Order\n(S0_I0_T0)', 'FF LIFO\n(S0_I1_T0)', 'BF LIFO\n(S1_I1_T0)', 'TH LIFO\n(S2_I1_T0)', 'System Malloc']
latencies = []
colors = ['#e74c3c', '#2980b9', '#27ae60', '#8e44ad', '#7f8c8d']

cids = ['S0_I0_T0', 'S0_I1_T0', 'S1_I1_T0', 'S2_I1_T0', 'system_malloc']
for cid in cids:
    match = [r for r in data if r['config_id'] == cid and r['workload'] == 'hole_reuse']
    latencies.append(float(match[0]['median_ns_op']) if match else 0.0)

ax1.bar(labels, latencies, color=colors, edgecolor='black', linewidth=0.5)
ax1.set_ylabel('Latency (ns/op)', fontsize=10, fontweight='bold')
ax1.set_title('Hole Reuse Latency (2 MB Arena)', fontsize=11, fontweight='bold')
ax1.grid(axis='y', linestyle='--', alpha=0.5)
for idx, v in enumerate(latencies):
    ax1.text(idx, v + 15, f"{v:.1f} ns", ha='center', fontsize=9, fontweight='bold')

# Subplot 2: Nodes Visited per Alloc in Hole Reuse
policy_labels = ['First-Fit\n(S0_I1_T0)', 'Best-Fit\n(S1_I1_T0)', 'Take-Head\n(S2_I1_T0)']
nodes = [9.00, 4.78, 9.00]
policy_colors = ['#2980b9', '#27ae60', '#8e44ad']

ax2.bar(policy_labels, nodes, color=policy_colors, edgecolor='black', linewidth=0.5)
ax2.set_ylabel('Mean Nodes Visited per Alloc', fontsize=10, fontweight='bold')
ax2.set_title('Search Efficiency: Nodes Visited (Hole Reuse)', fontsize=11, fontweight='bold')
ax2.grid(axis='y', linestyle='--', alpha=0.5)
for idx, v in enumerate(nodes):
    ax2.text(idx, v + 0.3, f"{v:.2f}", ha='center', fontsize=9, fontweight='bold')

plt.tight_layout()
plt.savefig('report/figures/hole_reuse_analysis.pdf', dpi=300)
plt.savefig('report/figures/hole_reuse_analysis.png', dpi=300)
plt.close()
print("Generated hole_reuse_analysis.pdf and .png")
