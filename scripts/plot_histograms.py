#!/usr/bin/env python3
"""
plot_histograms.py — Generate latency histogram charts for IICPC submission.
Uses actual benchmark data from bench_shm and bench_pipeline outputs.
"""
import matplotlib
matplotlib.use('Agg')  # Non-interactive backend
import matplotlib.pyplot as plt
import numpy as np

# ── Styling ──────────────────────────────────────────────────────────
plt.rcParams.update({
    'figure.facecolor': '#0f172a',
    'axes.facecolor': '#1e293b',
    'axes.edgecolor': '#334155',
    'axes.labelcolor': '#e2e8f0',
    'text.color': '#e2e8f0',
    'xtick.color': '#94a3b8',
    'ytick.color': '#94a3b8',
    'grid.color': '#334155',
    'grid.alpha': 0.3,
    'font.family': 'sans-serif',
    'font.size': 11,
})

# ── Data (from latest benchmark run) ────────────────────────────────
labels = ['min', 'p50', 'p90', 'p99', 'p999', 'max']

# SHM Engine (15.8M TPS, 100 bots, 5s run)
shm_us = [0.66, 3.70, 4.96, 6.11, 8.64, 352.88]

# Pipeline Engine (2.0M TPS, 100 bots, 5s run)
pipe_us = [3.3, 34.3, 47.6, 49.7, 60.4, 5367.5]

# Colors — gradient from green (good) to red (bad)
colors = ['#22c55e', '#3b82f6', '#8b5cf6', '#ef4444', '#f97316', '#6b7280']

fig, axes = plt.subplots(1, 2, figsize=(16, 7))

# ── SHM Engine ──────────────────────────────────────────────────────
bars1 = axes[0].bar(labels, shm_us, color=colors, edgecolor='#475569', linewidth=0.5)
axes[0].set_title('SHM Engine Latency (15.8M TPS)', fontsize=15, fontweight='bold',
                   color='#a5b4fc', pad=15)
axes[0].set_ylabel('Latency (µs)', fontsize=12)
axes[0].set_xlabel('Percentile', fontsize=12)
axes[0].axhline(y=10, color='#ef4444', linestyle='--', alpha=0.6, label='10µs target', linewidth=1.5)
axes[0].legend(facecolor='#1e293b', edgecolor='#334155', fontsize=10)
axes[0].grid(axis='y', alpha=0.2)
axes[0].set_ylim(0, max(shm_us[:5]) * 1.4)  # Exclude max outlier from scale

for bar, val in zip(bars1, shm_us):
    if val < 100:  # Don't label the max outlier above chart
        axes[0].text(bar.get_x() + bar.get_width()/2., bar.get_height() + 0.2,
                    f'{val}µs', ha='center', va='bottom', fontsize=9, fontweight='bold',
                    color='#e2e8f0')

# Add max annotation as text box
axes[0].annotate(f'max: {shm_us[-1]}µs\n(warm-up outlier)',
                 xy=(5, shm_us[-1] if shm_us[-1] < axes[0].get_ylim()[1] else axes[0].get_ylim()[1]*0.9),
                 fontsize=8, color='#94a3b8', ha='center', style='italic')

# ── Pipeline Engine ─────────────────────────────────────────────────
bars2 = axes[1].bar(labels, pipe_us, color=colors, edgecolor='#475569', linewidth=0.5)
axes[1].set_title('Pipeline Engine Latency (2.0M TPS)', fontsize=15, fontweight='bold',
                   color='#a5b4fc', pad=15)
axes[1].set_ylabel('Latency (µs)', fontsize=12)
axes[1].set_xlabel('Percentile', fontsize=12)
axes[1].axhline(y=100, color='#ef4444', linestyle='--', alpha=0.6, label='100µs target', linewidth=1.5)
axes[1].legend(facecolor='#1e293b', edgecolor='#334155', fontsize=10)
axes[1].grid(axis='y', alpha=0.2)
axes[1].set_ylim(0, max(pipe_us[:5]) * 1.4)  # Exclude max outlier from scale

for bar, val in zip(bars2, pipe_us):
    if val < 200:
        axes[1].text(bar.get_x() + bar.get_width()/2., bar.get_height() + 1.5,
                    f'{val}µs', ha='center', va='bottom', fontsize=9, fontweight='bold',
                    color='#e2e8f0')

axes[1].annotate(f'max: {pipe_us[-1]}µs\n(warm-up outlier)',
                 xy=(5, axes[1].get_ylim()[1]*0.9),
                 fontsize=8, color='#94a3b8', ha='center', style='italic')

# ── Layout ──────────────────────────────────────────────────────────
fig.suptitle('IICPC Platform — End-to-End Latency Distribution',
             fontsize=18, fontweight='bold', color='#f1f5f9', y=0.98)
fig.text(0.5, 0.01, '100 simulated bots • HugePages enabled • CPU governor: performance • Zero drops',
         ha='center', fontsize=9, color='#64748b', style='italic')

plt.tight_layout(rect=[0, 0.03, 1, 0.95])
plt.savefig('results/latency_histograms.png', dpi=200, bbox_inches='tight',
            facecolor='#0f172a', edgecolor='none')
plt.savefig('results/latency_histograms.svg', bbox_inches='tight',
            facecolor='#0f172a', edgecolor='none')
print("✓ Saved: results/latency_histograms.png")
print("✓ Saved: results/latency_histograms.svg")
