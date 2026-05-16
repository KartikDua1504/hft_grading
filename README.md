<p align="center">
  <h1 align="center">IICPC</h1>
  <p align="center"><strong>High-Frequency Trading Arena</strong></p>
  <p align="center">
    An institutional-grade benchmarking platform for competitive orderbook engineering.<br/>
    Submit your C++ matching engine. Get scored on correctness, throughput, and tail latency.
  </p>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Engine-14M_TPS-blueviolet?style=flat-square" />
  <img src="https://img.shields.io/badge/p99-0.8μs-cyan?style=flat-square" />
  <img src="https://img.shields.io/badge/Drops-0-green?style=flat-square" />
  <img src="https://img.shields.io/badge/Isolation-Firecracker_MicroVM-red?style=flat-square" />
  <img src="https://img.shields.io/badge/C%2B%2B-23-orange?style=flat-square" />
  <img src="https://img.shields.io/badge/License-MIT-lightgrey?style=flat-square" />
</p>

---

## What Is This?

IICPC is a **competitive programming platform** where teams write C++ orderbook matching engines and submit them for automated benchmarking. The platform:

1. **Compiles** your code with `-O3 -march=native -static` (static glibc linking)
2. **Isolates** execution in a **Firecracker MicroVM** — separate kernel per contestant, zero shared attack surface
3. **Blasts** millions of deterministic orders through your engine
4. **Validates** every fill against a shadow reference orderbook
5. **Scores** you on correctness (40%), throughput (30%), and p99 latency (30%)

Think [IMC Prosperity](https://prosperity.imc.com/) meets [Codeforces](https://codeforces.com/), but for systems programming.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        IICPC PLATFORM                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────┐    ┌──────────┐    ┌───────────────────────────┐ │
│  │ SvelteKit│───▶│ FastAPI  │───▶│     Redis Job Queue       │ │
│  │ Frontend │◀──▶│  + JWT   │    │  (FIFO, BRPOP blocking)   │ │
│  │ :5173    │ WS │  :8000   │    └───────────┬───────────────┘ │
│  └──────────┘    └──────────┘                │                  │
│                                              ▼                  │
│                                    ┌─────────────────┐         │
│                                    │ Submission Worker│         │
│                                    │  (async loop)    │         │
│                                    └────────┬────────┘         │
│                                             │                   │
│                                             ▼                   │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │              FIRECRACKER MICROVM SANDBOX                   │  │
│  │                                                          │  │
│  │  1. g++ -O3 -std=c++23 -march=native -static -flto      │  │
│  │  2. Firecracker MicroVM (separate kernel per contestant) │  │
│  │  3. Pre-warmed snapshot resume (<5ms boot)               │  │
│  │  4. OrderBlaster → deterministic order stream            │  │
│  │  5. ShadowOrderbook → correctness validation             │  │
│  │  6. HDR Histogram → latency percentiles                  │  │
│  │  7. Score = 0.4*correctness + 0.3*throughput + 0.3*lat   │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                     │
│  │ QuestDB  │  │ Redpanda │  │  Redis   │                     │
│  │ ILP:9009 │  │ Kafka    │  │ Sorted   │                     │
│  │ metrics  │  │ streaming│  │ Sets     │                     │
│  └──────────┘  └──────────┘  └──────────┘                     │
└─────────────────────────────────────────────────────────────────┘
```

---

## Performance

Benchmarked on consumer hardware (no FPGA, no kernel bypass):

| Metric | Value |
|---|---|
| **Engine Throughput** | 13.99M transactions/sec |
| **p50 Latency** | 0.35 µs |
| **p99 Latency** | 0.81 µs |
| **p99.9 Latency** | 1.5 µs |
| **Drops** | 0 |
| **Arena Allocation** | 0.3 ns/alloc |
| **Ring Buffer (SPSC)** | 809.9M ops/sec |
| **Spinlock Cycle** | 8.6 ns |

---

## Prerequisites

| Dependency | Version | Purpose |
|---|---|---|
| **Linux** | 5.15+ | KVM, cgroups v2, io_uring, HugePages |
| **KVM** | — | `/dev/kvm` required for Firecracker |
| **Firecracker** | 1.15+ | MicroVM hypervisor |
| **GCC/G++** | 12+ | C++23 support, `-march=native` |
| **CMake** | 3.20+ | Build system |
| **Python** | 3.10+ | FastAPI backend |
| **Node.js** | 18+ | SvelteKit frontend |
| **Redis** | 7+ | Job queue, leaderboard |
| **Docker** | 24+ | Infrastructure services (optional) |

---

## Quick Start

### 1. Clone & Build the C++ Engine

```bash
git clone <repo-url> IICPC
cd IICPC

# Build all C++ targets
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

# Verify build
./pipeline_e2e           # Should show ~14M TPS
./run_contest --help     # Contest runner
```

### 2. Start Infrastructure

**Option A: Docker Compose (recommended)**

```bash
cd infra/docker
docker compose up -d

# Services started:
#   Redis      → localhost:6379
#   QuestDB    → localhost:9000 (web), :9009 (ILP)
#   Redpanda   → localhost:19092 (Kafka), :8080 (console)
```

**Option B: Local Redis only (minimal)**

```bash
redis-server --daemonize yes
```

### 3. Start the API Server

```bash
# Install Python dependencies
cd web/backend
pip install -r requirements.txt

# Start the server
uvicorn main:app --host 0.0.0.0 --port 8000

# Verify
curl http://localhost:8000/api/health
# → {"status":"healthy","redis":true,"version":"1.0.0"}
```

### 4. Start the Frontend

```bash
cd web/frontend
npm install
npm run dev -- --port 5173

# Open http://localhost:5173
```

### 5. Run E2E Tests

```bash
# With API server + Redis running:
./scripts/e2e_test.sh

# Expected: 10/10 PASS
```

### 6. Setup Firecracker MicroVM Isolation

Firecracker provides **hardware-level isolation** — each contestant gets their own Linux kernel. No shared kernel attack surface.

```bash
# Verify KVM is available
ls -la /dev/kvm

# Verify Firecracker is installed
firecracker --version  # Should show v1.15+

# The kernel + rootfs are pre-configured in infra/firecracker/:
#   vmlinux.bin         — Minimal Linux kernel (virtio_blk built-in)
#   base_rootfs.ext4    — Alpine Linux 3.8 minimal rootfs

# Create a pre-warmed snapshot (Strategy 1: <5ms resume)
./scripts/create_snapshot.sh

# This creates:
#   infra/firecracker/snapshots/snapshot_state  (25K)
#   infra/firecracker/snapshots/snapshot_mem    (256M)
```

**Security model:** Contestant code runs inside a Firecracker microVM with:
- Separate kernel instance (no shared kernel attack surface)
- 2 vCPUs, 256MB RAM hard limit
- No network access
- Read-only rootfs with injected binary
- Automatic cleanup on timeout/crash

---

## System Hardening (Production)

For deterministic benchmark results, run the hardening script:

```bash
sudo ./scripts/harden_determinism.sh
```

This configures:
- CPU governor → `performance` (no frequency scaling)
- Turbo Boost → disabled (no frequency jitter)
- HugePages → 512 × 2MB = 1GB pre-allocated
- THP → disabled (no compaction stalls)
- Swappiness → 0 (no swap interference)
- NMI watchdog → disabled (fewer interrupts)
- Network buffers → 16MB (no packet drops)
- cgroups v2 → subtree delegation enabled

---

## Project Structure

```
IICPC/
├── core/                    # Low-level primitives
│   └── include/core/
│       ├── arena.hpp            # HugePage bump allocator (0.3 ns/alloc)
│       ├── spinlock.hpp         # Ticket spinlock (8.6 ns)
│       ├── ring_buffer.hpp      # Lock-free SPSC (809M ops/sec)
│       ├── seqlock.hpp          # SeqLock reader-writer
│       ├── hdr_histogram.hpp    # Zero-alloc latency tracking
│       ├── hot_path_asm.hpp     # RDTSC, prefetch, fence intrinsics
│       └── cpu_affinity.hpp     # CPU pinning utilities
│
├── exchange/                # Matching engine
│   └── include/exchange/
│       ├── orderbook.hpp        # SoA price-time priority orderbook
│       ├── match_engine.hpp     # Full matching engine with PnL
│       ├── shadow_orderbook.hpp # Correctness validator
│       └── market_data_gen.hpp  # Deterministic Ornstein-Uhlenbeck
│
├── loadgen/                 # Deterministic order blaster
│   └── include/loadgen/
│       └── order_blaster.hpp    # Seeded PRNG order generator
│
├── orchestrator/            # Contest orchestration
│   ├── include/orchestrator/
│   │   └── contest_runner.hpp   # Compile → Boot → Blast → Score
│   └── src/
│       └── run_contest.cpp      # CLI contest runner
│
├── sandbox/                 # Isolation layer
│   └── include/sandbox/
│       ├── sandbox_bridge.hpp   # UDS host↔contestant bridge
│       ├── compiler_service.hpp # g++ compilation service
│       └── firecracker_manager.hpp  # Firecracker MicroVM lifecycle
│
├── sdk/                     # Contestant SDK
│   └── include/sdk/
│       └── protocol.hpp         # Binary message protocol
│
├── pipeline/                # Telemetry pipeline
│   └── include/pipeline/
│       └── metrics_publisher.hpp  # QuestDB ILP publisher
│
├── web/
│   ├── backend/             # FastAPI server
│   │   ├── main.py              # Auth, upload, queue, WebSocket
│   │   └── requirements.txt
│   └── frontend/            # SvelteKit UI
│       └── src/routes/
│           ├── +page.svelte         # Landing page
│           ├── auth/+page.svelte    # Login/Register
│           └── dashboard/
│               ├── +layout.svelte   # Sidebar layout
│               ├── +page.svelte     # Overview
│               ├── submit/          # Code submission
│               └── leaderboard/     # Rankings
│
├── scripts/
│   ├── firecracker_sandbox.sh  # Firecracker MicroVM sandbox (primary)
│   ├── create_snapshot.sh      # Pre-warm + snapshot base VM
│   ├── sandbox_run.sh          # cgroups v2 fallback runner
│   ├── harden_determinism.sh   # System tuning
│   ├── e2e_test.sh             # End-to-end API tests
│   ├── build_rootfs.sh         # Build Alpine rootfs image
│   └── dev.sh                  # Dev environment starter
│
├── infra/
│   ├── firecracker/
│   │   ├── vmlinux.bin          # Minimal Linux kernel
│   │   ├── base_rootfs.ext4     # Alpine rootfs (30MB)
│   │   └── snapshots/           # Pre-warmed VM snapshots
│   └── docker/
│       ├── docker-compose.yml   # Full stack
│       └── Dockerfile.api       # API container
│
├── bench/                   # Benchmarks
│   ├── bench_arena.cpp
│   ├── bench_ringbuf.cpp
│   ├── bench_blaster.cpp
│   └── bench_pipeline.cpp
│
├── docs/
│   └── ARCHITECTURE_BOOK.md # Comprehensive technical manifesto
│
└── CMakeLists.txt           # Build configuration
```

---

## Scoring Formula

```
Score = 0.4 × Correctness + 0.3 × Throughput + 0.3 × Latency
```

| Component | Weight | How It's Measured |
|---|---|---|
| **Correctness** | 40% | Shadow orderbook validates every fill. Price-time priority violations = instant penalty. |
| **Throughput** | 30% | Orders processed per second under sustained load. Normalized against baseline. |
| **Latency** | 30% | p99 round-trip response time. Measured at the wire boundary. Lower = better. |

---

## API Reference

| Method | Endpoint | Auth | Description |
|---|---|---|---|
| `GET` | `/api/health` | No | System health + Redis status |
| `POST` | `/api/auth/register` | No | Register team (returns JWT) |
| `POST` | `/api/auth/login` | No | Login (returns JWT) |
| `POST` | `/api/submit` | JWT | Upload `.cpp` file for benchmarking |
| `GET` | `/api/job/{id}` | JWT | Poll job status + results |
| `GET` | `/api/leaderboard` | No | Top 50 teams by score |
| `WS` | `/ws/live` | No | Real-time leaderboard updates |

---

## For Contestants

### Writing Your Engine

Your code must implement a matching engine that:
1. Connects to the gateway via Unix Domain Socket
2. Receives `MarketUpdate` messages (deterministic price stream)
3. Sends `OrderEntry` / `CancelRequest` messages
4. Receives `Fill` / `OrderAck` messages
5. Maximizes correctness, throughput, and minimizes latency

```cpp
// Minimal skeleton
#include "sdk/protocol.hpp"

int main(int argc, char* argv[]) {
    // Parse --gateway <socket_path>
    // Connect to UDS
    // Read MarketUpdate messages
    // Submit orders
    // Process fills
    return 0;
}
```

### Compilation

Your code is compiled with:
```bash
g++ -O3 -std=c++23 -march=native -flto -static -DNDEBUG
```

### Constraints

| Resource | Limit |
|---|---|
| Max file size | 50 MB |
| VM memory | 256 MB (Firecracker hard limit) |
| VM vCPUs | 2 (isolated) |
| Timeout | 120 seconds |
| Network | None (air-gapped) |
| Kernel | Separate instance per run |

---

## Tech Stack

| Layer | Technology | Why |
|---|---|---|
| **Engine** | C++23 | Zero-overhead abstractions, cache-line control |
| **Memory** | HugePages (2MB) | Eliminates TLB misses |
| **Sync** | Lock-free SPSC | 809M ops/sec, no contention |
| **I/O** | io_uring | Batched syscalls, zero kernel transitions |
| **Metrics** | QuestDB (ILP) | 1.4M rows/sec ingestion, zero serialization |
| **Queue** | Redis | BRPOP FIFO, sorted set leaderboard |
| **Streaming** | Redpanda | Kafka-compatible, lower latency |
| **API** | FastAPI | Async Python, WebSocket support |
| **Frontend** | SvelteKit | Reactive, compiled, fast |
| **Isolation** | Firecracker MicroVM | Separate kernel, hardware-enforced limits |

---

## License

MIT

---

<p align="center">
  <strong>IICPC</strong> — Where nanoseconds matter.
</p>
