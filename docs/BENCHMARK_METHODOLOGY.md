# Benchmarking Methodology

## Environment

- **CPU**: Intel Core i7-12700H (6P+8E cores, P-cores @ 4.7GHz, E-cores @ 3.5GHz)
- **RAM**: 32GB DDR5-4800
- **OS**: Gentoo Linux, kernel 6.12.58
- **Compiler**: GCC 15.2 with `-O3 -march=native -mtune=native -flto=auto -fno-plt -mprefer-vector-width=256`
- **SIMD**: AVX2 (`__attribute__((target("avx2")))` per-function), AVX-512 when available
- **HugePages**: 512 × 2MB pre-allocated (1GB)
- **CPU Governor**: `performance` (verified via `scaling_governor`)
- **THP**: `madvise` mode (no background compaction)
- **Isolation**: No `isolcpus` on dev machine. AWS metal uses `isolcpus=4-11 nohz_full=4-11 rcu_nocbs=4-11`

## Warm-Up Protocol

1. **TSC Calibration**: First 100ms measures TSC frequency via `clock_gettime` cross-reference
2. **TLB Priming**: Arena pre-faulted via `madvise(MADV_POPULATE_WRITE)` before measurement
3. **I-cache Warm**: First 1 second of each benchmark is warm-up (discarded from stats)
4. **Branch Predictor**: Warm-up phase trains the branch predictor on the hot loop

## Statistical Method

- **Duration**: Each benchmark runs for 5–10 seconds minimum (configurable via `--duration`)
- **Latency Tracking**: HDR Histogram with 2 significant digits, range 1ns–10s
- **Throughput**: Measured at wire boundary (`total_sends / elapsed_time`)
- **Drop Detection**: `sends == receives` enforced; any mismatch is a test failure
- **Percentiles**: min, p50, p90, p99, p99.9, max reported via HDR Histogram

## Timing Methodology

All timestamps use the x86 RDTSC instruction (Time Stamp Counter):

```
Start: LFENCE; RDTSC (serialized — no reordering)
End:   RDTSCP; LFENCE (serialized — waits for prior instructions)
```

TSC is calibrated once at startup to convert ticks → nanoseconds. On Intel Alder Lake with
`constant_tsc` and `nonstop_tsc` flags, TSC ticks at a constant rate regardless of frequency
scaling, providing sub-nanosecond precision.

**Why not `clock_gettime(CLOCK_MONOTONIC)`?**

`clock_gettime` is a vDSO call (~20ns), which is 60× slower than inline RDTSC (~0.3ns).
At 15M measurements/sec, this would consume 300ms/sec just on timing overhead.

## Hardware Counters (perf stat)

Key metrics captured via `perf stat -d`:

| Counter | Blaster (9.7M OPS) | SHM (15.8M TPS) |
|---------|---------------------|------------------|
| IPC | 2.45 | 0.32* |
| L1 Cache Miss | 0.16% | 4.59% |
| LLC (L3) Miss | 37% of LLC loads | 0.01% of LLC loads |
| Branch Miss | 2.31% | 0.25% |
| Context Switches | 0 | 0 |
| Page Faults | 145 (setup only) | 167 (setup only) |

\* SHM IPC is low because 82.5% of cycles are backend-bound (spin-waiting on SPSC ring).
This is expected — the consumer thread spends most time polling `write_pos.load(acquire)`,
which is a cache-line transfer from the producer core.

## Reproducibility

```bash
# 1. Build
cd IICPC/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)

# 2. Harden (optional but recommended)
sudo bash scripts/harden_determinism.sh

# 3. Run
./bench_arena                              # Arena allocator
./bench_ringbuf                            # SPSC ring buffer
./bench_blaster --duration 10              # Order generation
./bench_shm --duration 10 --bots 1000      # Full E2E SHM
./bench_pipeline --duration 10 --bots 1000 # Full E2E pipe

# 4. Hardware counters
perf stat -d ./bench_blaster --duration 5
perf stat -d ./bench_shm --duration 5 --bots 100

# 5. Flamegraphs
perf record -g --call-graph dwarf -F 4999 -- ./bench_blaster --duration 5
perf script | stackcollapse-perf.pl | flamegraph.pl > flamegraph.svg
```

## Test Machine vs Production Target

| Aspect | Dev Machine (i7-12700H) | Production (c8i.metal-48xl) |
|--------|------------------------|-----------------------------|
| Cores | 6P + 8E (hybrid) | 48 (homogeneous Xeon) |
| isolcpus | No | Yes (cores 4-11) |
| NUMA | Single socket | Single socket |
| HugePages | 512 × 2MB | 1024 × 2MB |
| Expected TPS improvement | Baseline | 1.5–2× (dedicated cores, no E-core interference) |

## Profile-Guided Optimization (PGO)

PGO workflow for maximum compiler optimization:

```bash
# Step 1: Generate (instruments binary with profiling counters)
cmake .. -DCMAKE_BUILD_TYPE=Release -DIICPC_PGO_PHASE=generate
make -j$(nproc)

# Step 2: Train (run representative workload — generates .gcda profiles)
./bench_shm --duration 10 --bots 100
./bench_pipeline --duration 10 --bots 100

# Step 3: Use (rebuilds with profile data — optimized branch layout, I-cache)
cmake .. -DCMAKE_BUILD_TYPE=Release -DIICPC_PGO_PHASE=use
make -j$(nproc)
```

**Expected improvement:** 10–20% throughput, 5–15% latency reduction.

PGO enables:
- **Branch layout optimization**: Hot branches fall through, cold branches are branched-to
- **I-cache optimization**: Hot code is packed together, cold code sinks to end of function
- **Indirect call devirtualization**: Based on observed call targets during profiling

## FPGA Simulation Methodology

- **Simulator**: Verilator 5.046 (cycle-accurate)
- **Clock**: 250 MHz (4ns period)
- **Architecture**: 3-stage feed-forward pipeline (Decode → Match → Emit)
- **Throughput measurement**: `orders / elapsed_simulation_time`, reported in M orders/sec
- **Multi-frequency projection**: Throughput scales linearly with clock frequency for pipeline architectures
- **Waveform capture**: VCD format, viewable in GTKWave
- **Build command**: `cd fpga && make sim_match`
