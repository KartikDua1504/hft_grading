# Building a High-Performance Orderbook Testing Arena

## A Complete Engineering Reference

**From First Principles to 14 Million Transactions Per Second**

---

# Table of Contents

1. [The Problem Statement](#chapter-1-the-problem-statement)
2. [Architecture Overview](#chapter-2-architecture-overview)
3. [The Memory Foundation: Arena Allocators & Huge Pages](#chapter-3-the-memory-foundation)
4. [The Type System: Cache-Line Awareness](#chapter-4-the-type-system)
5. [Assembly on the Hot Path](#chapter-5-assembly-on-the-hot-path)
6. [Synchronization: Spinlocks, SeqLocks & Lock-Free Queues](#chapter-6-synchronization)
7. [The CRTP Pattern: Compile-Time Polymorphism](#chapter-7-the-crtp-pattern)
8. [The Wire Protocol](#chapter-8-the-wire-protocol)
9. [The Order Blaster: Deterministic Load Generation](#chapter-9-the-order-blaster)
10. [The SPSC Ring Buffer](#chapter-10-the-spsc-ring-buffer)
11. [The Shadow Orderbook: Correctness Validation](#chapter-11-the-shadow-orderbook)
12. [The Sandbox Bridge: Unix Domain Sockets](#chapter-12-the-sandbox-bridge)
13. [CPU Core Isolation & Determinism](#chapter-13-cpu-core-isolation)
14. [Firecracker over Docker](#chapter-14-firecracker-over-docker)
15. [The Telemetry Pipeline: HDR Histograms & QuestDB](#chapter-15-the-telemetry-pipeline)
16. [Infrastructure Choices: Redis, Redpanda, QuestDB](#chapter-16-infrastructure-choices)
17. [The Web Platform: FastAPI + SvelteKit](#chapter-17-the-web-platform)
18. [The Build System & Deployment](#chapter-18-the-build-system)
19. [Complete File Blueprint](#chapter-19-complete-file-blueprint)

---

# Chapter 1: The Problem Statement

## What Are We Building?

We are **not** building an exchange. We are building a **testing arena** — a platform that takes a contestant's orderbook matching engine (written in C++), sandboxes it, and stress-tests it with millions of deterministic orders to measure three things:

1. **Correctness** (40%): Does the engine match orders according to price-time priority? Every fill is validated against a deterministic reference implementation.
2. **Throughput** (30%): How many orders per second can the engine process under sustained load?
3. **Latency** (30%): What is the 99th percentile round-trip time from order submission to response?

## Why This Is Hard

A naive implementation would use `std::map` for the orderbook, `std::mutex` for synchronization, `new`/`delete` for allocation, and `gettimeofday()` for timing. Such an implementation would achieve perhaps 100,000 orders per second with 50µs latency. We need **100x better**.

The difficulty is not algorithmic — a matching engine is conceptually simple (two sorted lists, match when they overlap). The difficulty is **mechanical sympathy**: making the code cooperate with the CPU's cache hierarchy, branch predictor, memory controller, and OS scheduler to achieve nanosecond-level performance.

Every design choice documented here exists to eliminate a specific source of latency or jitter. Nothing is accidental.

---

# Chapter 2: Architecture Overview

## System Topology

```
┌─────────────────────────────────────────────────────────────────┐
│                        HOST MACHINE                             │
│  (AWS c8i.metal-48xl — 48 cores, bare metal, no hypervisor)     │
│                                                                 │
│  ┌──────────┐   ┌──────────────┐   ┌─────────────────────┐     │
│  │  Cores   │   │   Cores      │   │      Cores          │     │
│  │  0-3     │   │   4-7        │   │      8-11           │     │
│  │  ─────── │   │   ────────── │   │      ──────────     │     │
│  │  OS +    │   │   Order      │   │   Contestant Code   │     │
│  │  Docker  │   │   Blaster    │   │   (Firecracker µVM) │     │
│  │  NGINX   │   │   (10M OPS)  │   │                     │     │
│  │  FastAPI │   │              │   │                     │     │
│  └──────────┘   └──────┬───────┘   └──────────┬──────────┘     │
│                        │                      │                 │
│                        │  Unix Domain Socket   │                │
│                        │  (zero-copy IPC)      │                │
│                        └──────────┬────────────┘                │
│                                   │                             │
│                     ┌─────────────▼───────────────┐             │
│                     │     Cores 12-15             │             │
│                     │     ─────────────           │             │
│                     │     Shadow Orderbook        │             │
│                     │     (Reference Validator)   │             │
│                     └─────────────────────────────┘             │
│                                                                 │
│  ┌──────────────────────────────────────────────────┐           │
│  │           Telemetry Pipeline                     │           │
│  │  SPSC Ring → HDR Histogram → QuestDB + Redis     │           │
│  └──────────────────────────────────────────────────┘           │
└─────────────────────────────────────────────────────────────────┘
```

## Data Flow (One Complete Contest Run)

1. **Submit**: User uploads `orderbook.cpp` via web UI → FastAPI → Redis FIFO queue
2. **Compile**: Worker pops job → `CompilerService` compiles with GCC 14, `-O2 -march=x86-64-v3`, static linking with musl libc
3. **Boot**: `FirecrackerManager` boots a µVM with the compiled binary and a 7MB Alpine rootfs
4. **Connect**: Contestant binary connects to host via Unix Domain Socket (`SandboxBridge`)
5. **Blast**: `OrderBlaster` generates 10M+ deterministic orders/sec, sent through the bridge
6. **Shadow**: The exact same order stream feeds the `ShadowOrderbook` on the host
7. **Validate**: Contestant responses are diffed against shadow — fills must match exactly
8. **Score**: Composite score = 0.4×correctness + 0.3×throughput + 0.3×(1/p99_latency)
9. **Publish**: Score → Redis sorted set (leaderboard) + QuestDB (time-series telemetry)
10. **Display**: SvelteKit frontend pulls leaderboard via FastAPI, live updates via WebSocket

---

# Chapter 3: The Memory Foundation

**File**: `core/include/core/arena.hpp`, `core/src/arena.cpp`

## Why Not malloc/new?

`malloc()` is a general-purpose allocator. It maintains free lists, handles fragmentation, uses mutexes for thread safety, and makes syscalls (`brk`/`mmap`) when it runs out of memory. Each of these operations adds latency and non-determinism:

| Operation | Latency |
|-----------|---------|
| `malloc(64)` (cached) | ~25ns |
| `malloc(64)` (uncached, needs `brk`) | ~1000ns |
| Arena bump allocation | **0.3ns** |

That's an **83x improvement** in the best case, **3333x** in the worst.

## How the Arena Works

The arena is a **bump allocator**. It gets one giant block of memory up front (via `mmap`), maintains a single pointer (`offset_`), and "allocates" by advancing the pointer:

```cpp
void* allocate_raw(size_t bytes, size_t alignment) noexcept {
    size_t aligned_offset = align_up(offset_, alignment);
    if (aligned_offset + bytes > size_) return nullptr;
    void* ptr = base_ + aligned_offset;
    offset_ = aligned_offset + bytes;
    return ptr;
}
```

There is **no free()**. When a contest run ends, the entire arena is reset to zero in O(1):

```cpp
void reset() noexcept { offset_ = 0; }
```

This eliminates fragmentation, eliminates use-after-free bugs, and eliminates the overhead of maintaining a free list. The trade-off is that you cannot free individual objects — but in a benchmarking pipeline that runs in phases (setup → blast → collect → reset), this is exactly what we want.

## Why Huge Pages?

On x86-64, the CPU translates virtual addresses to physical addresses using a **Translation Lookaside Buffer (TLB)**. With standard 4KB pages:

- A 2GB arena = 524,288 pages = 524,288 TLB entries needed
- Intel Alder Lake has ~1,536 L1 DTLB entries and ~2,048 L2 STLB entries
- **Result**: Constant TLB misses. Each miss costs ~7ns (L2 TLB) to ~100ns (page table walk)

With 2MB huge pages:

- A 2GB arena = 1,024 pages = 1,024 TLB entries needed
- This fits entirely in the L2 STLB
- **Result**: Zero TLB misses after warmup

The arena requests huge pages via `mmap` with `MAP_HUGETLB | MAP_HUGE_2MB`:

```cpp
void* ptr = mmap(nullptr, size,
    PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
    -1, 0);
```

If the kernel doesn't have enough huge pages reserved (configured via `vm.nr_hugepages`), it falls back gracefully to standard 4KB pages. The `bootstrap.sh` script reserves 1,024 huge pages (2GB):

```bash
sysctl -w vm.nr_hugepages=1024
```

## All Allocations are 64-Byte Aligned

Every allocation from the arena lands on a cache line boundary. This prevents **false sharing** (two unrelated data items sharing a cache line, causing cache coherency ping-pong between cores) and enables the use of aligned SSE/AVX instructions.

---

# Chapter 4: The Type System — Cache-Line Awareness

**File**: `core/include/core/types.hpp`

## The 64-Byte Rule

Intel CPUs fetch memory in 64-byte cache lines. This is the fundamental unit of memory the hardware works with. Our entire type system is designed around this:

```cpp
inline constexpr size_t CACHE_LINE_SIZE = 64;
```

### PaddedAtomic — Preventing False Sharing

When two threads write to different variables that happen to live on the same cache line, the CPU's cache coherency protocol (MESI/MESIF) forces both cores to invalidate and reload that line. This is called **false sharing** and can cost 50-100ns per occurrence.

Our `PaddedAtomic<T>` wraps a `std::atomic<T>` in its own cache line:

```cpp
template<typename T>
struct alignas(CACHE_LINE_SIZE) PaddedAtomic {
    std::atomic<T> value{0};
    char _pad[CACHE_LINE_SIZE - sizeof(std::atomic<T>)];
};
static_assert(sizeof(PaddedAtomic<uint64_t>) == 64);
```

The `static_assert` is critical — it catches at compile time if padding is wrong. If `sizeof(atomic<uint64_t>)` ever changes across compilers, we get a build error, not a subtle performance regression.

### LatencySample — The Telemetry Atom

```cpp
struct alignas(8) LatencySample {
    TscTicks send_tsc;    // 8 bytes
    TscTicks recv_tsc;    // 8 bytes
    uint32_t bot_id;      // 4 bytes
    uint32_t seq_num;     // 4 bytes
};
static_assert(sizeof(LatencySample) == 24);
```

This is exactly 24 bytes — intentionally NOT padded to 64. Why? Because LatencySamples flow through the SPSC ring buffer at 14M/sec. Padding them to 64 bytes would waste 40 bytes × 14M = 560MB/sec of memory bandwidth. At 24 bytes, nearly 3 samples fit per cache line, making sequential reads extremely efficient.

### RingBufferElement Concept (C++20)

```cpp
template<typename T>
concept RingBufferElement = std::is_trivially_copyable_v<T>
                         && std::is_trivially_destructible_v<T>
                         && (sizeof(T) <= 256);
```

This C++20 concept constrains what types can go through our ring buffer. `trivially_copyable` means `memcpy` is valid (no copy constructors, no vtables). `trivially_destructible` means no destructor calls needed. The size cap prevents accidentally putting large objects in the ring.

---

# Chapter 5: Assembly on the Hot Path

**File**: `core/include/core/hot_path_asm.hpp`

## Why Use Inline Assembly At All?

Compilers are excellent at optimization, but they have constraints. They must obey the C++ abstract machine model, they insert barriers conservatively, and they use general-purpose instruction patterns. In three specific cases, hand-written assembly produces measurably better results:

### Case 1: RDTSC — Serialized Timestamp

We need nanosecond-precision timing. The x86 `RDTSC` instruction reads the Time Stamp Counter — a 64-bit register that increments at the CPU's base clock frequency (~2.7 GHz on our target hardware). One tick ≈ 0.37 nanoseconds.

**The problem with `__rdtsc()`**: The compiler intrinsic does not serialize. Out-of-order execution can move `RDTSC` ahead of the instructions you're trying to measure. Intel's recommended pattern uses `LFENCE; RDTSC` at the start and `RDTSCP; LFENCE` at the end.

```asm
; Start measurement (full serialization)
lfence          ; Complete ALL prior instructions before reading TSC
rdtsc           ; Read TSC → edx:eax

; End measurement (rdtscp already serializes on the read side)
rdtscp          ; Read TSC, wait for all prior instructions
lfence          ; Prevent later instructions from moving before this
```

Our implementation:

```cpp
[[nodiscard]] __attribute__((always_inline))
inline uint64_t rdtsc_serialized() noexcept {
    uint32_t lo, hi;
    asm volatile(
        "lfence\n\t"
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi)
        :
        : "memory"
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
```

The `"memory"` clobber acts as a compiler barrier — it tells GCC/Clang that this instruction may read/write any memory, preventing the compiler from reordering loads/stores across it.

For the highest-precision measurement pair, we use `CPUID` instead of `LFENCE` for the start timestamp. `CPUID` is a fully serializing instruction — it drains the entire instruction pipeline:

```cpp
inline uint64_t timestamp_begin() noexcept {
    uint32_t lo, hi;
    asm volatile(
        "cpuid\n\t"        // Full pipeline drain (~200 cycles)
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi)
        : "a"(0)
        : "rbx", "rcx", "memory"
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
```

Cost: `CPUID` is ~200 cycles vs `LFENCE`'s ~25 cycles. We use `CPUID` only for the benchmark start, not inside the hot loop.

### Case 2: Cache-Line Copy — 4 Cycles for 64 Bytes

Protocol messages (OrderEntry, Fill) are exactly 64 bytes. The compiler's `memcpy` for 64 bytes typically generates `rep movsb`, which has a startup cost of ~15 cycles on modern Intel. We use four SSE2 `movdqa` loads + four stores:

```cpp
inline void copy_cacheline(void* dst, const void* src) noexcept {
    asm volatile(
        "movdqa   (%[src]),    %%xmm0\n\t"    // 16 bytes
        "movdqa 16(%[src]),    %%xmm1\n\t"    // 16 bytes
        "movdqa 32(%[src]),    %%xmm2\n\t"    // 16 bytes
        "movdqa 48(%[src]),    %%xmm3\n\t"    // 16 bytes
        "movdqa %%xmm0,   (%[dst])\n\t"
        "movdqa %%xmm1, 16(%[dst])\n\t"
        "movdqa %%xmm2, 32(%[dst])\n\t"
        "movdqa %%xmm3, 48(%[dst])\n\t"
        :
        : [dst] "r"(dst), [src] "r"(src)
        : "xmm0", "xmm1", "xmm2", "xmm3", "memory"
    );
}
```

`movdqa` requires 16-byte alignment (the 'a' stands for 'aligned'). Since all our protocol messages are `alignas(64)`, this is guaranteed. The 4 loads can execute in parallel on modern Intel (2 load ports), so the effective throughput is ~4 cycles for 64 bytes.

### Case 3: Non-Temporal Stores — Bypassing the Cache

When writing telemetry data that won't be re-read soon, regular stores pollute the L1/L2 cache and evict useful hot data. Non-temporal stores (`movntdq`) write directly to the write-combine buffer, bypassing the cache hierarchy:

```cpp
inline void store_cacheline_nt(void* dst, const void* src) noexcept {
    // ... load with movdqa ...
    "movntdq %%xmm0,   (%[dst])\n\t"     // Non-temporal store
    // ... etc ...
}
```

After a batch of NT stores, an `sfence` is required to ensure they're visible to other cores:

```cpp
inline void nt_store_fence() noexcept {
    asm volatile("sfence" ::: "memory");
}
```

### Case 4: PAUSE — Spin-Wait Efficiency

The `PAUSE` instruction tells the CPU "I'm in a spin loop." On Alder Lake, it delays ~140 cycles while reducing power consumption and memory bus contention. Without `PAUSE`, a tight spin loop saturates the memory bus with speculative loads:

```cpp
inline void spin_pause_n(uint32_t count) noexcept {
    asm volatile(
        "1:\n\t"
        "pause\n\t"
        "sub $1, %[cnt]\n\t"
        "jnz 1b\n\t"
        : [cnt] "+r"(count) : :
    );
}
```

### Case 5: Prefetch Hints

When traversing SoA arrays, we know 8 iterations ahead which memory we'll need. Hardware prefetching works for sequential access but fails for strided or indirect access:

```cpp
inline void prefetch_l1_read(const void* addr) noexcept {
    asm volatile("prefetcht0 (%[addr])" : : [addr] "r"(addr));
}
```

`prefetcht0` loads into L1 (4-cycle access). `prefetcht1` loads into L2 (12-cycle access). `prefetchnta` loads with "non-temporal" hint (used once, don't pollute cache).


---

# Chapter 6: Synchronization Primitives

**File**: `core/include/core/spinlock.hpp`

## Why Not std::mutex?

`std::mutex` uses the Linux `futex` syscall. When contended, it context-switches the thread — the kernel saves all registers, flushes the TLB, loads another thread's context, and resumes. This costs 10,000–50,000 nanoseconds. On our hot path, that's the equivalent of processing 10,000 orders worth of latency wasted on a single lock.

Worse, `futex` involves the kernel scheduler. On an `isolcpus` core where we've explicitly removed our thread from the scheduler's purview, any syscall re-introduces the OS into the picture.

Our hierarchy:

1. **Lock-free** (preferred) — atomics with acquire/release semantics
2. **Spinlock** (when exclusion is needed) — ticket-based, fair
3. **SeqLock** (reader-writer, single writer) — zero cost for readers

## Ticket Spinlock

A naive test-and-set spinlock is **unfair** — thread A might acquire the lock 100 times while thread B starves. Our ticket spinlock assigns monotonic tickets:

```cpp
class alignas(CACHE_LINE_SIZE) TicketSpinlock {
    alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> next_ticket_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> now_serving_{0};
};
static_assert(sizeof(TicketSpinlock) == 2 * CACHE_LINE_SIZE);
```

**Critical**: `next_ticket_` and `now_serving_` are on **separate cache lines**. If they shared a line, every `fetch_add` on `next_ticket_` by the acquirer would invalidate the cache line containing `now_serving_`, which the releaser is trying to update — cache coherency ping-pong.

Lock acquisition:

```cpp
void lock() noexcept {
    const uint32_t my_ticket = next_ticket_.fetch_add(1, relaxed);
    while (now_serving_.load(acquire) != my_ticket) {
        cpu_pause();  // PAUSE instruction — see Chapter 5
    }
}
```

The `relaxed` ordering on `fetch_add` is safe because we only need the ticket value, not visibility of prior stores. The `acquire` on the spin loop synchronizes with the `release` in `unlock()`.

## SeqLock — Zero-Cost Reads

For market data that is written by one thread and read by many, mutexes are wasteful. A SeqLock uses a sequence counter:

- Writer increments to ODD (writing), writes data, increments to EVEN (done)
- Reader reads sequence, reads data, re-reads sequence. If it changed or was odd, retry.

```cpp
// Writer
void write_lock() noexcept {
    seq_.store(seq_.load(relaxed) + 1, release);  // Goes odd
    compiler_barrier();
}
void write_unlock() noexcept {
    compiler_barrier();
    seq_.store(seq_.load(relaxed) + 1, release);  // Goes even
}

// Reader — ZERO ATOMIC RMW OPERATIONS
uint32_t read_begin() const noexcept {
    uint32_t seq;
    do {
        seq = seq_.load(acquire);
    } while (seq & 1);  // Retry if writer is active
    return seq;
}
bool read_valid(uint32_t start_seq) const noexcept {
    compiler_barrier();
    return seq_.load(acquire) == start_seq;
}
```

The reader path has **zero** `fetch_add` or `compare_exchange` — only `load(acquire)`. This means readers never cause cache line invalidations on the writer's core. For a market data feed read by 100 bots, this eliminates 100× cache coherency traffic per update.

## MPSCQueue — Multi-Producer Single-Consumer

For aggregating results from multiple bot threads:

```cpp
template<typename T, uint32_t Capacity>
class MPSCQueue {
    alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> head_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> tail_{0};
};
```

Producers use `compare_exchange_weak` (CAS loop) to claim a slot. The consumer is single-threaded, so it only needs a `load(acquire)` on head and a `store(release)` on tail. The `Capacity` is constrained to be a power of 2 so that modulo reduces to a bitmask (`& MASK`), which is a single-cycle AND instruction vs. a ~30-cycle division.

---

# Chapter 7: The CRTP Pattern — Compile-Time Polymorphism

**File**: `core/include/core/crtp_engine.hpp`

## The Problem with Virtual Functions

Traditional OOP uses `virtual` functions and vtables for polymorphism. On every call to a virtual method:

1. Load the vtable pointer from the object (1 memory access)
2. Index into the vtable to find the function pointer (1 memory access)
3. Indirect branch to the function (branch prediction miss possible)
4. **Compiler cannot inline** — it doesn't know the target at compile time

On Alder Lake:
- vtable lookup: ~5ns (L1 hit) to ~40ns (L3, if evicted by other data)
- Indirect branch misprediction: ~15 cycle penalty
- Inlining blocked: the compiler can't see through the indirection

At 14M operations/sec, every nanosecond matters. 40ns × 14M = 560ms/sec wasted.

## CRTP: The Curiously Recurring Template Pattern

```cpp
template<typename Derived>
class EngineBase {
    Derived& self() noexcept {
        return static_cast<Derived&>(*this);
    }

    void send_all(BotFleet& fleet) noexcept {
        for (size_t i = 0; i < fleet.count; ++i) {
            self().do_send_impl(fleet, i);  // Resolved at COMPILE TIME
        }
    }
};

// Usage:
class PipelineEngine : public EngineBase<PipelineEngine> {
    void do_send_impl(BotFleet& fleet, size_t i) noexcept {
        // Concrete implementation — compiler INLINES this
    }
};
```

The `static_cast<Derived&>(*this)` is resolved by the compiler at template instantiation. There is no vtable, no indirection, no function pointer. The call to `do_send_impl` is replaced with the actual function body inline.

**Cost: literally zero.** The generated assembly is identical to if you'd written the code directly.

We use three CRTP bases:
- `EngineBase<D>`: For IO engines (send/recv loop)
- `ExchangeBase<D>`: For exchange-side processing
- `TelemetryBase<D>`: For telemetry consumers

Each is marked `IICPC_HOT IICPC_FLATTEN`:
- `__attribute__((hot))`: Tells the compiler to place this code in a hot section of the binary, improving instruction cache locality
- `__attribute__((flatten))`: Inlines ALL function calls within this function, recursively

---

# Chapter 8: The Wire Protocol

**File**: `sdk/include/sdk/protocol.hpp`

## Design Constraints

Every protocol message is:
- **Fixed-size**: Either 32 or 64 bytes (half or full cache line)
- **Trivially copyable**: `memcpy`-safe, no constructors/destructors
- **Cache-line aligned**: `alignas(64)` or `alignas(32)`
- **Native endian**: No byte-swapping (same machine, no network)
- **No serialization**: No Protobuf, no JSON, no FlatBuffers — raw POD structs

### Why No Protobuf/FlatBuffers?

Protobuf serialization for a 64-byte message: ~200ns encode + ~150ns decode = **350ns**
Raw struct copy: ~4ns (one `copy_cacheline`)

That's **87x faster**. Since all communication is same-machine (host ↔ µVM via UDS), there's no endianness concern and no schema evolution requirement.

### Message Layout

```
Exchange → Strategy:
  MarketUpdate  (64B) — Top-of-book data, sequence-numbered
  OrderAck      (32B) — Acceptance/rejection of an order
  Fill          (64B) — Execution notification with price/qty
  CancelAck     (32B) — Cancel confirmation

Strategy → Exchange:
  OrderEntry    (64B) — New order submission
  CancelRequest (32B) — Order cancellation

Control:
  SessionStart  (64B) — Match configuration
  SessionEnd    (64B) — Final PnL and statistics
  Heartbeat     (32B) — Keep-alive
```

### Type Discrimination

The first byte of every message is `MsgType` (a `uint8_t` enum). The receiver peeks at byte 0 to determine the type and size:

```cpp
MsgType peek_msg_type(const void* buf) noexcept {
    return *static_cast<const MsgType*>(buf);
}

constexpr uint32_t msg_size(MsgType type) noexcept {
    switch (type) {
        case MsgType::MARKET_UPDATE:  return 64;
        case MsgType::ORDER_ACK:      return 32;
        case MsgType::FILL:           return 64;
        // ... etc
    }
}
```

This is a compile-time dispatch — `msg_size` is `constexpr`, so the compiler can evaluate it statically when the type is known.

### Price Representation

All prices are `int64_t` with a fixed multiplier of 10,000:

```cpp
inline constexpr int64_t PRICE_MULTIPLIER = 10000;
// $100.00 = 1,000,000 internal units
```

Why not `double`? Floating point has rounding errors. In financial systems, $0.10 + $0.10 + $0.10 ≠ $0.30 in IEEE 754. Integer arithmetic is exact. The multiplier gives us 4 decimal places of precision ($0.0001 resolution).

---

# Chapter 9: The Order Blaster — Deterministic Load Generation

**File**: `loadgen/include/loadgen/order_blaster.hpp`

## What It Does

The Order Blaster is the "firehose" — it generates a realistic stream of exchange orders at 10M+ per second. It simulates:
- **Limit orders** (60%): Buy/sell at specific prices around a random-walking midpoint
- **Market orders** (20%): Immediate execution at any price
- **Cancel requests** (20%): Remove previously placed limit orders

## Determinism: The xorshift64 PRNG

**Critical requirement**: For fair scoring, every contestant must receive the **exact same order sequence**. This means the random number generator must be deterministic — same seed produces same sequence, always.

We use xorshift64, a non-cryptographic PRNG that produces high-quality randomness in 3 XOR + shift operations:

```cpp
uint64_t next_raw() noexcept {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return rng_;
}
```

Why not `std::mt19937`? Mersenne Twister maintains a 2.5KB state array and uses complex tempering. xorshift64 has a single `uint64_t` state and takes ~1ns per call. At 10M calls/sec, this saves 25ms/sec.

For Gaussian-distributed price noise (realistic market simulation), we use the Box-Muller transform:

```cpp
double next_gaussian() noexcept {
    double u1 = next_uniform();
    double u2 = next_uniform();
    return sqrt(-2.0 * log(u1)) * cos(2.0 * π * u2);
}
```

## SoA State Tracking

Active orders are tracked in Structure-of-Arrays (SoA) format:

```cpp
struct BlasterState {
    uint32_t* order_ids;      // [id, id, id, ...]
    int64_t*  order_prices;   // [price, price, price, ...]
    Side*     order_sides;    // [BUY, SELL, BUY, ...]
    int32_t*  order_qtys;     // [100, 50, 75, ...]
    bool*     order_active;   // [true, false, true, ...]
};
```

Why SoA instead of Array-of-Structs (AoS)?

When scanning for an active order to cancel, we iterate `order_active[]`. In SoA, these booleans are contiguous in memory — 64 booleans per cache line. In AoS, each boolean would be separated by ~30 bytes of other fields, meaning only ~2 per cache line. **SoA is 32x more cache-efficient for this scan.**

---

# Chapter 10: The SPSC Ring Buffer

**File**: `core/include/core/ring_buffer.hpp`

## LMAX Disruptor Architecture

Our ring buffer follows the LMAX Disruptor pattern (invented for the London Multi-Asset Exchange):

- **Single Producer, Single Consumer** (SPSC) — no locks needed
- **Bounded** — fixed capacity, power-of-2 for fast modulo
- **Shared memory compatible** — can be placed in `memfd_create` regions

### Memory Layout

```
[RingBufferHeader: 3 cache lines]
  [write_pos: 1 cache line]     ← Producer owns this line
  [read_pos:  1 cache line]     ← Consumer owns this line
  [metadata:  1 cache line]     ← Read-only after init
[T[Capacity] data array]
```

The `static_assert` that catches false sharing at compile time:

```cpp
static_assert(
    offsetof(RingBufferHeader, read_pos) -
    offsetof(RingBufferHeader, write_pos) >= CACHE_LINE_SIZE,
    "CRITICAL: write_pos and read_pos MUST be on separate cache lines"
);
```

### Memory Ordering: Acquire-Release Only

We **never** use `seq_cst` (sequential consistency) on the hot path. `seq_cst` requires an `MFENCE` instruction on x86, which flushes the store buffer (~20ns). Acquire-Release is sufficient and free on x86 (loads are naturally acquire, stores are naturally release):

```cpp
// Producer: publish data
bool try_push(const T& item) noexcept {
    uint64_t w = write_pos.load(relaxed);        // Only I write this
    uint64_t r = read_pos.load(acquire);          // Sync with consumer
    if (w - r >= Capacity) return false;           // Full
    data[w & MASK] = item;                         // Write BEFORE publishing
    write_pos.store(w + 1, release);              // Publish: consumer can now see the data
    return true;
}
```

The `release` on `write_pos.store` guarantees that the data write (`data[w & MASK] = item`) is visible to the consumer before the updated write position. This is the **happens-before** relationship that makes the ring buffer correct without locks.

### Power-of-2 Capacity: Why?

`w & MASK` replaces `w % Capacity`. Modulo is a division instruction (~30 cycles on Alder Lake). Bitwise AND is 1 cycle. At 14M operations/sec, this saves 400ms/sec.

```cpp
template<typename T, size_t Capacity>
    requires RingBufferElement<T> && (is_power_of_2(Capacity))
class SPSCRingBuffer { ... };
```

The `requires` clause (C++20 concepts) enforces this at compile time.

### Shared Memory: memfd_create

For cross-process IPC (host ↔ Firecracker VM), the ring buffer can be placed in shared memory:

```cpp
int fd = syscall(SYS_memfd_create, "iicpc_ring",
                 MFD_HUGETLB | MFD_HUGE_2MB);
ftruncate(fd, total_size);
void* ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0);
```

`memfd_create` creates an anonymous file in memory — no filesystem overhead. Both processes `mmap` the same fd, giving them a shared memory region with zero-copy semantics.

---

# Chapter 11: The Shadow Orderbook

**File**: `exchange/include/exchange/shadow_orderbook.hpp`

## The Correctness Problem

We blast 10 million orders at the contestant's code. How do we know if their matching engine got the right answers?

The Shadow Orderbook is a **deterministic reference implementation** that runs on the host, processing the **exact same order stream** as the contestant. Because the Order Blaster is seeded (`seed=12345`), the shadow receives identical input and produces the expected set of fills.

When the contestant returns their fills, we diff:

| Condition | Consequence |
|-----------|-------------|
| Fill matches expected price + qty | ✅ Correct |
| Fill at wrong price | Wrong price penalty |
| Fill with wrong quantity | Wrong qty penalty |
| Expected fill not received | Missing fill (severe) |
| Unexpected fill received | Extra fill / phantom trade (instant fail) |
| Out-of-order fills | Priority violation (instant fail) |

## The ACK vs FILL Bug (And Why It Mattered)

The contestant's response struct has a `response_type` field:

```cpp
struct ContestantResponse {
    uint32_t order_id;
    MsgType  response_type;    // ORDER_ACK or FILL
    // ...
    int64_t  fill_price;
    int32_t  fill_qty;
};
```

The original code validated **every** response as a fill:

```cpp
// BUG: Treats ACKs as fills → inflated extra_fills count
while (bridge.pop_response(resp)) {
    shadow.validate_fill(resp.order_id, resp.fill_price, resp.fill_qty);
}
```

An ORDER_ACK has `fill_price = 0` and `fill_qty = 0`. The shadow has no expected fill with those values, so every ACK was counted as an "extra fill" → zero correctness score.

The fix:

```cpp
while (bridge.pop_response(resp)) {
    if (resp.response_type == MsgType::FILL) {
        shadow.validate_fill(resp.order_id, resp.fill_price, resp.fill_qty);
    } else if (resp.response_type == MsgType::ORDER_ACK) {
        shadow.result_mut().total_acks++;  // Count but don't validate
    }
}
```

## Scoring Formula

```cpp
double correctness_score() const noexcept {
    if (total_expected == 0) return 1.0;
    double base = (double)correct_fills / (double)total_expected;
    double penalty = priority_errors * 0.1 + (extra_fills + missing_fills) * 0.01;
    return max(0.0, base - penalty);
}
```

A single priority violation (matching a worse-priced order before a better-priced one) costs 10% of the score. This is intentionally harsh — price-time priority is the fundamental invariant of any orderbook.


---

# Chapter 12: The Sandbox Bridge — Unix Domain Sockets

**File**: `sandbox/include/sandbox/sandbox_bridge.hpp`

## Why Unix Domain Sockets Over TCP?

The contestant's code runs in a Firecracker µVM on the **same physical machine**. TCP would route through the kernel's TCP/IP stack — SYN/ACK handshake, Nagle's algorithm, congestion control, checksumming. All unnecessary overhead for local IPC.

Unix Domain Sockets (UDS) bypass the network stack entirely:

| Feature | TCP loopback | UDS |
|---------|-------------|-----|
| Handshake | 3-way SYN/ACK | Single `connect()` |
| Checksumming | Yes (even loopback) | No |
| Nagle buffering | Yes (must disable) | No |
| Congestion control | Yes (cwnd management) | No |
| Context switches | ~4 per send/recv | ~2 per send/recv |
| Latency (64B msg) | ~5-8µs | ~1-2µs |

### Non-Blocking Sends with Backpressure

The bridge uses `MSG_DONTWAIT | MSG_NOSIGNAL`:

```cpp
ssize_t n = send(client_fd_, order.data, order.size,
                 MSG_DONTWAIT | MSG_NOSIGNAL);
```

- `MSG_DONTWAIT`: If the kernel send buffer is full (contestant is too slow), return `EAGAIN` immediately instead of blocking. We record this as a **drop**.
- `MSG_NOSIGNAL`: If the contestant crashes and the socket closes, don't kill the host with SIGPIPE.

Drops are penalized in the scoring. This creates backpressure: a slow contestant will lose orders and get a lower throughput score.

### Timing Boundary

The latency clock is defined by two precise TSC reads:

```
CLOCK START: asm_hot::rdtsc_serialized() → immediately before send()
CLOCK STOP:  asm_hot::rdtscp_end()       → immediately after recv()
```

Everything between these two points — kernel socket buffer, UDS transmission, VM processing, kernel socket buffer again — is the contestant's measured latency. This is the fairest possible measurement: it includes all overhead the contestant cannot control (UDS latency) equally for all contestants.

## Pending TSC Tracking

When we send an order, we store the TSC timestamp in a circular buffer indexed by order ID. When we receive the response, we look up the matching send TSC to compute round-trip latency:

```cpp
// On send:
pending_tsc_[stats_.orders_sent % PENDING_TSC_SIZE] = send_tsc;
pending_oid_[stats_.orders_sent % PENDING_TSC_SIZE] = order_id;

// On recv:
for (uint32_t i = 0; i < PENDING_TSC_SIZE; ++i) {
    if (pending_oid_[i] == resp->order_id) {
        resp->send_tsc = pending_tsc_[i];
        pending_oid_[i] = 0;  // Consumed
        break;
    }
}
```

---

# Chapter 13: CPU Core Isolation & Determinism

**File**: `core/include/core/cpu_affinity.hpp`, `infra/terraform/bootstrap.sh`

## The Jitter Problem

On a default Linux system, the OS scheduler can:
1. Migrate your thread to a different core (cache cold start: ~1000 cycles)
2. Schedule a kernel thread on your core (timer interrupts: every 4ms)
3. Run interrupt handlers on your core (NIC, disk, timer)
4. Run RCU callbacks on your core

Each of these adds microseconds of non-deterministic latency. In our benchmarks, this shows up as p99 spikes.

## Three-Layer Isolation

### Layer 1: isolcpus (Kernel Boot Parameter)

```bash
# In bootstrap.sh / GRUB:
GRUB_CMDLINE_LINUX_DEFAULT="isolcpus=4-15 nohz_full=4-15 rcu_nocbs=4-15"
```

- `isolcpus=4-15`: Removes cores 4-15 from the default scheduler. No userspace process will run there unless explicitly pinned.
- `nohz_full=4-15`: Disables the timer tick on these cores when only one task is running. Normally, the kernel interrupts every core every 4ms (250 Hz) — this stops that.
- `rcu_nocbs=4-15`: Offloads RCU (Read-Copy-Update) callbacks to other cores. RCU is the kernel's lock-free synchronization mechanism; its callbacks can take microseconds.

### Layer 2: sched_setaffinity (Process Pinning)

```cpp
static bool pin_this_thread(int core) noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;
}
```

This pins a thread to a specific core. Combined with `isolcpus`, the thread has the core **entirely to itself**.

### Layer 3: SCHED_FIFO (Real-Time Scheduling)

```cpp
static bool set_realtime(int priority = 90) noexcept {
    struct sched_param param{};
    param.sched_priority = priority;
    return sched_setscheduler(0, SCHED_FIFO, &param) == 0;
}
```

`SCHED_FIFO` is a real-time scheduling policy. A FIFO thread preempts ALL normal threads and never yields the CPU unless it blocks. Priority 90 (out of 99) ensures we preempt everything except critical kernel threads.

### Core Map

```cpp
struct CoreMap {
    static constexpr int BLASTER_CORE     = 4;   // Order generation
    static constexpr int BLASTER_CORE_END = 7;
    static constexpr int CONTEST_CORE     = 8;   // Contestant's code
    static constexpr int CONTEST_CORE_END = 11;
    static constexpr int SHADOW_CORE      = 12;  // Shadow validation
    static constexpr int SHADOW_CORE_END  = 15;
};
```

Cores 0-3 are left to the OS for Docker, NGINX, FastAPI, and system services. They are intentionally NOT isolated — the OS needs somewhere to run.

## CPU Governor: Performance Mode

```bash
cpupower frequency-set -g performance
```

The `performance` governor locks all cores at maximum frequency. The default `powersave` governor dynamically scales frequency based on load — a frequency transition takes ~10µs and introduces non-deterministic latency during the transition.

---

# Chapter 14: Firecracker Over Docker

**File**: `sandbox/include/sandbox/firecracker_manager.hpp`

## Why Not Docker?

Docker containers share the host kernel. This creates three problems for a competition platform:

### Problem 1: Kernel Attack Surface

A contestant's C++ code runs as a native binary. With kernel sharing, a kernel exploit in the contestant's code affects the host. Docker's seccomp profile blocks ~44 syscalls, but new exploits emerge constantly. The attack surface is the entire Linux kernel syscall table (~450 syscalls).

Firecracker gives each contestant a **separate kernel instance**. The attack surface is reduced to the Firecracker VMM's ~25 host syscalls via seccomp-bpf.

### Problem 2: Resource Isolation Guarantees

Docker uses cgroups for resource limits. cgroups are enforced by the kernel scheduler, which samples at tick intervals (4ms by default). A contestant could spike to 200% CPU for 3.9ms before cgroups react.

Firecracker uses KVM hardware virtualization. The CPU's VT-x/VT-d hardware enforces vCPU time slices at the hardware level. There is no sampling delay.

### Problem 3: Deterministic Networking

Docker's network namespace adds overhead to every packet. Even with `--network=host`, iptables/nftables rules add latency. Our UDS bridge bypasses all of this — the socket file is shared via the Firecracker VM's vsock or a mapped virtio-block device.

## Boot Time

Firecracker boots in ~125ms. A Docker container starts in ~500ms. But the real win is that Firecracker's boot is **deterministic** — no systemd, no D-Bus, no udev. The guest kernel goes straight to init, which is the contestant's binary.

## The Alpine Rootfs

```bash
# build_rootfs.sh creates a 7MB ext4 image with:
# - musl libc (statically linked contestant binary needs nothing else)
# - /dev/console, /dev/null, /dev/zero (minimal device nodes)
# - /init → contestant binary
```

The rootfs is read-only. Each contestant gets a copy-on-write overlay (Firecracker's block device snapshot). This means booting 100 VMs simultaneously doesn't require 100 copies of the rootfs.

---

# Chapter 15: The Telemetry Pipeline

**Files**: `core/include/core/hdr_histogram.hpp`, `telemetry/include/telemetry/consumer.hpp`, `pipeline/include/pipeline/metrics_publisher.hpp`

## HDR Histogram — Sub-Microsecond Percentile Tracking

Standard histograms use fixed-width bins. For latency measurement spanning 100ns to 100ms (6 orders of magnitude), you'd need millions of bins for 1ns resolution.

HDR (High Dynamic Range) Histogram uses **logarithmically-scaled bins**. Low values get fine resolution; high values get coarser resolution. Our implementation:

- Range: 1ns to 1 second
- Significant digits: 3 (0.1% precision)
- Memory: ~42KB (fits entirely in L1 cache)

This lets us compute accurate p50, p90, p99, p99.9, and max from 14M samples/sec with zero allocation.

## Data Flow

```
Engine Hot Loop → [LatencySample] → SPSC Ring → Telemetry Consumer
                                                      ↓
                                              HDR Histogram (in-memory)
                                                      ↓
                                              ┌───────┴───────┐
                                              ↓               ↓
                                          QuestDB          Redis
                                     (time-series)    (leaderboard)
```

The SPSC ring decouples the hot path from the telemetry consumer. The producer never blocks on telemetry — if the ring is full, samples are dropped (counted as "drops" in the report). The consumer runs on a separate core and processes samples at its own pace.

## QuestDB: Why Not PostgreSQL?

QuestDB is a columnar time-series database optimized for append-only ingestion. Comparison:

| Feature | PostgreSQL | QuestDB |
|---------|-----------|---------|
| Ingestion rate | ~50K rows/sec | ~4M rows/sec |
| Query: "p99 latency last 5s" | ~100ms (B-tree scan) | ~1ms (columnar scan) |
| Storage: 1B latency rows | ~80GB | ~8GB (columnar compression) |
| Wire protocol | Custom binary | InfluxDB Line Protocol (TCP) |

The InfluxDB Line Protocol (ILP) is trivial to implement — it's just a TCP socket writing ASCII lines:

```
latency,team=quantum_traders,job=abc123 p50=350,p99=810,tps=13985699 1715529600000000000
```

No driver needed. No ORM. Just `snprintf` + `send()`.

---

# Chapter 16: Infrastructure Choices

## Redis Over PostgreSQL (For Leaderboard + Auth)

The leaderboard is a sorted set of ~100 teams by score. This is Redis's `ZSET` — O(log N) insert, O(N) range scan, entirely in-memory.

PostgreSQL would add: connection pooling, query parsing, B-tree index maintenance, WAL writes, vacuum. For a 100-entry leaderboard that changes ~10 times/minute, this is massive overkill.

Redis also serves as the job queue (`LPUSH`/`BRPOP` for FIFO) and session store (team registration). One service, three use cases, zero configuration.

```yaml
redis:
  image: redis:7-alpine
  command: >
    redis-server
    --maxmemory 256mb
    --maxmemory-policy allkeys-lru
    --appendonly yes
```

`appendonly yes` enables AOF (Append-Only File) persistence — Redis writes every mutation to disk. If the container restarts, it replays the AOF and restores state. No data loss.

## Redpanda Over Kafka (For Event Streaming)

Redpanda is a Kafka-compatible streaming platform written in C++ with a single binary. Kafka is written in Java and requires a JVM + ZooKeeper (or KRaft).

| Feature | Kafka | Redpanda |
|---------|-------|----------|
| Language | Java (JVM) | C++ (native) |
| Dependencies | JVM + ZK/KRaft | Single binary |
| Memory | 1-4GB JVM heap | 512MB |
| Tail latency | ~10ms (GC pauses) | ~1ms (no GC) |
| Config files | server.properties, log4j, zk | Single YAML |
| Docker image | ~600MB | ~200MB |

The JVM's garbage collector creates **non-deterministic latency spikes**. A GC pause in the metrics pipeline could block telemetry publication for 50-100ms. Redpanda, being C++ with no GC, provides consistent sub-millisecond latencies.

We use Redpanda's Kafka-compatible API (port 19092) for durable event streaming of contest results. Each contest run produces a message on the `iicpc.results` topic, consumed by the leaderboard updater.

```yaml
redpanda:
  command:
    - --smp=1          # Single core (we don't need throughput)
    - --memory=512M    # Minimal footprint
    - --overprovisioned # Don't try to reserve exclusive cores
    - --mode=dev-container
```

---

# Chapter 17: The Web Platform

## FastAPI (Python Backend)

**File**: `web/backend/main.py`

FastAPI handles the "slow path" — HTTP requests from users. It's chosen for:
- **async/await**: Non-blocking Redis and file I/O
- **Pydantic**: Request/response validation
- **Auto-generated OpenAPI docs**: `/docs` endpoint for free
- **WebSocket support**: Native, for live leaderboard updates

The backend is a thin orchestration layer:

```
POST /api/auth/register → Redis HSET → JWT token
POST /api/auth/login    → Redis HGET → verify → JWT token
POST /api/submit        → Stream to disk → Redis LPUSH queue
GET  /api/leaderboard   → Redis ZREVRANGE → sorted entries
WS  /ws/live            → Pub/sub broadcast on score updates
```

### The Background Worker

A single `asyncio.Task` runs the submission worker:

```python
async def submission_worker():
    while True:
        result = await redis_pool.brpop(QUEUE_KEY, timeout=5)
        if result is None: continue
        _, job_id = result
        # Compile → Run → Score → Update leaderboard
```

`BRPOP` is a blocking pop — it sleeps until a job arrives, consuming zero CPU. Only one contest runs at a time (Firecracker requires exclusive core access).

### Auth: JWT + SHA-256

Passwords are hashed with SHA-256 (sufficient for a competition; production would use bcrypt). JWTs expire in 24 hours. The frontend stores the token in `localStorage` and sends it via `Authorization: Bearer <token>` header.

## SvelteKit (Frontend)

**Files**: `web/frontend/src/routes/*.svelte`

### Why SvelteKit?

- **Compiled**: Svelte compiles to vanilla JS at build time — no runtime framework overhead
- **Reactive**: `$state()` runes for fine-grained reactivity without a virtual DOM
- **SSR/SPA hybrid**: Server-side rendering for initial load, client-side navigation after

### Page Architecture

| Route | Purpose |
|-------|---------|
| `/` | Cinematic landing with typewriter quote animation |
| `/auth` | Login/Register with demo-mode fallback |
| `/dashboard` | Two-card hub (Submit / Leaderboard) |
| `/dashboard/submit` | Drag-drop file upload with validation |
| `/dashboard/leaderboard` | Live rankings with scoring breakdown |

### Vite Proxy (Development)

```typescript
// vite.config.ts
server: {
  proxy: {
    '/api': { target: 'http://127.0.0.1:8000', changeOrigin: true },
    '/ws':  { target: 'ws://127.0.0.1:8000', ws: true },
  },
}
```

In development, Vite intercepts `/api/*` requests from the browser and forwards them to FastAPI on port 8000. In production, NGINX handles this.

---

# Chapter 18: The Build System & Deployment

## CMake Build (C++ Engine)

**File**: `CMakeLists.txt`

```cmake
set(CMAKE_CXX_STANDARD 23)              # C++23 for concepts, ranges
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=x86-64-v3 -mtune=native")
```

`-march=x86-64-v3` targets AVX2/BMI2 — available on all server CPUs since Haswell (2013). This enables the compiler to use 256-bit SIMD instructions for loops and data movement. `-mtune=native` optimizes instruction scheduling for the exact CPU we're compiling on.

Why `-O2` and not `-O3`? `-O3` enables aggressive loop unrolling and vectorization that can increase code size, causing **instruction cache pressure**. At 14M ops/sec, I-cache misses matter. `-O2` provides a better code-size/performance trade-off for our workload.

## Terraform (AWS Deployment)

**File**: `infra/terraform/main.tf`

```hcl
resource "aws_instance" "arena" {
  ami           = "ami-0c7217cdde317cfec"  # Ubuntu 24.04 LTS
  instance_type = "c8i.metal-48xl"          # 48 cores, bare metal
  key_name      = var.key_pair_name
  user_data     = file("${path.module}/bootstrap.sh")
}
```

### Why c8i.metal?

1. **Bare metal**: No hypervisor. Firecracker is itself a VMM — running it inside another VM (nested virtualization) adds latency and is sometimes unsupported.
2. **48 cores**: Enough for OS (4) + contest (12) + 32 for parallel contests.
3. **Intel Ice Lake**: Deterministic TSC (constant_tsc, nonstop_tsc flags) — the TSC ticks at a constant rate regardless of frequency scaling.

### bootstrap.sh — Zero-Touch Provisioning

The entire infrastructure is provisioned by a single script that runs as EC2 user-data:

```
[1/10] System packages (GCC 14, CMake, Ninja)
[2/10] Docker (for Redis, QuestDB, Redpanda)
[3/10] Node.js 22 (for SvelteKit)
[4/10] Performance tuning (hugepages, isolcpus, CPU governor)
[5/10] Firecracker binary + jailer
[6/10] Build C++ engine (cmake + ninja)
[7/10] Build Alpine rootfs (for contestant VMs)
[8/10] Start Docker services
[9/10] Python venv + FastAPI dependencies
[10/10] NGINX reverse proxy + systemd service
```

After `terraform apply`, a judge can SSH in and the platform is ready.

---

# Chapter 19: Complete File Blueprint

## Directory Tree

```
IICPC/
├── CMakeLists.txt                          # Master build — 16 targets
│
├── core/                                   # Foundation library
│   ├── include/core/
│   │   ├── types.hpp                       # Cache-line types, PaddedAtomic, LatencySample
│   │   ├── arena.hpp                       # HugePageArena bump allocator
│   │   ├── hot_path_asm.hpp                # Inline asm: RDTSC, copy_cacheline, prefetch
│   │   ├── compiler_hints.hpp              # LIKELY/UNLIKELY, prefetch, PAUSE, barriers
│   │   ├── spinlock.hpp                    # TicketSpinlock, SeqLock, MPSCQueue
│   │   ├── ring_buffer.hpp                 # SPSC ring + shared memory helpers
│   │   ├── crtp_engine.hpp                 # EngineBase, ExchangeBase, TelemetryBase
│   │   ├── tsc.hpp                         # TSC calibration (GHz → ns conversion)
│   │   ├── cpu_affinity.hpp                # Core pinning, SCHED_FIFO, CoreMap
│   │   ├── hdr_histogram.hpp               # HDR histogram for percentile tracking
│   │   └── flat_map.hpp                    # Open-addressing hash map
│   └── src/
│       ├── arena.cpp                       # mmap + hugepage allocation
│       └── hdr_histogram.cpp               # Histogram percentile computation
│
├── sdk/                                    # Contestant SDK
│   ├── include/sdk/
│   │   ├── protocol.hpp                    # Wire protocol (all message types)
│   │   ├── strategy_sdk.hpp                # Base class for contestant strategies
│   │   └── gateway_client.hpp              # UDS client for connecting to host
│   ├── src/
│   │   └── strategy_main.cpp               # Contestant entry point (main)
│   └── examples/
│       └── example_mm.cpp                  # Example market maker strategy
│
├── exchange/                               # Exchange simulation
│   ├── include/exchange/
│   │   ├── orderbook.hpp                   # SoA price-time priority orderbook
│   │   ├── shadow_orderbook.hpp            # Deterministic reference (validation)
│   │   ├── match_engine.hpp                # Match + fill generation
│   │   ├── gateway.hpp                     # Exchange gateway (message routing)
│   │   └── market_data_gen.hpp             # Synthetic market data
│   └── src/
│       └── exchange_local.cpp              # Local exchange for testing
│
├── loadgen/                                # Load generation
│   ├── include/loadgen/
│   │   ├── order_blaster.hpp               # 10M OPS deterministic order generator
│   │   ├── bot_fleet.hpp                   # SoA array of simulated bots
│   │   ├── pipeline_engine.hpp             # Pipe-based engine (CRTP)
│   │   ├── io_engine.hpp                   # epoll-based engine (CRTP)
│   │   ├── shm_engine.hpp                  # Shared-memory engine (CRTP)
│   │   ├── ultra_engine.hpp                # Ultra-low-latency engine (CRTP)
│   │   └── payload_gen.hpp                 # Market-realistic payload generation
│   └── src/
│       ├── bot_fleet.cpp
│       ├── io_engine.cpp
│       └── payload_gen.cpp
│
├── sandbox/                                # Contestant isolation
│   └── include/sandbox/
│       ├── sandbox_bridge.hpp              # Host↔VM UDS IPC bridge
│       ├── firecracker_manager.hpp         # Firecracker µVM lifecycle
│       └── compiler_service.hpp            # GCC compilation of .cpp submissions
│
├── orchestrator/                           # Contest execution
│   ├── include/orchestrator/
│   │   └── contest_runner.hpp              # Full pipeline: compile→boot→blast→score
│   └── src/
│       ├── run_contest.cpp                 # CLI: run a single contest
│       ├── orchestrator_server.cpp         # gRPC server for remote contest control
│       ├── integrated_worker.cpp           # Combined worker + engine
│       └── worker_agent.cpp                # Agent that pulls from Redis queue
│
├── telemetry/                              # Metrics collection
│   ├── include/telemetry/
│   │   └── consumer.hpp                    # SPSC ring consumer → HDR → QuestDB
│   └── src/
│       └── consumer.cpp
│
├── pipeline/                               # End-to-end integration
│   ├── include/pipeline/
│   │   └── metrics_publisher.hpp           # QuestDB ILP + Redis publisher
│   └── src/
│       └── pipeline_e2e.cpp                # Full pipeline benchmark
│
├── bench/                                  # Benchmarks
│   ├── bench_arena.cpp                     # Arena allocator perf
│   ├── bench_blaster.cpp                   # Order blaster throughput
│   ├── bench_ringbuf.cpp                   # SPSC ring SPSC throughput
│   ├── bench_pipeline.cpp                  # Pipeline TPS
│   ├── bench_shm.cpp                       # Shared memory ring perf
│   ├── bench_ultra.cpp                     # Ultra engine perf
│   └── validate_checkpoint1.cpp            # Validation test
│
├── web/                                    # Web platform
│   ├── backend/
│   │   ├── main.py                         # FastAPI: auth, submit, leaderboard, WS
│   │   └── requirements.txt
│   └── frontend/
│       ├── vite.config.ts                  # Vite proxy config
│       └── src/
│           ├── app.css                     # Design system (dark theme)
│           ├── lib/
│           │   ├── auth.ts                 # JWT store
│           │   └── quotes.ts              # Rotating quote pool
│           └── routes/
│               ├── +page.svelte            # Cinematic landing
│               ├── +layout.svelte          # Root layout
│               ├── auth/+page.svelte       # Login/Register
│               └── dashboard/
│                   ├── +page.svelte        # Dashboard hub
│                   ├── submit/+page.svelte # Code upload
│                   └── leaderboard/+page.svelte # Rankings
│
├── infra/                                  # Infrastructure
│   ├── docker/
│   │   └── docker-compose.yml              # Redis + QuestDB + Redpanda
│   └── terraform/
│       ├── main.tf                         # AWS c8i.metal provisioning
│       └── bootstrap.sh                    # Zero-touch server setup
│
├── scripts/                                # Utilities
│   ├── build_rootfs.sh                     # Alpine rootfs for Firecracker
│   ├── dev.sh                              # Local dev: FastAPI + SvelteKit
│   └── test_firecracker.sh                 # Firecracker smoke test
│
└── tests/
    └── sample_orderbook.cpp                # Reference orderbook implementation
```

## Measured Performance (Benchmark Results)

| Component | Metric | Measured |
|-----------|--------|----------|
| Arena Allocator | Allocation speed | 0.3 ns/alloc |
| TicketSpinlock | Lock/unlock (uncontended) | 8.6 ns |
| SeqLock | Write cycle | 0.5 ns |
| Order Blaster | Sustained throughput (5s) | 9.96M OPS |
| SPSC Ring (single-thread) | Push + pop | 1.2 ns/op (810M ops/sec) |
| SPSC Ring (cross-core) | Producer + consumer | 16.0M ops/sec |
| Pipeline E2E | Full telemetry pipeline | 13.99M TPS |
| Pipeline p50 | Median latency | 0.35 µs |
| Pipeline p99 | 99th percentile | 0.81 µs |
| Pipeline p99.9 | 99.9th percentile | 1.5 µs |
| Pipeline drops | Lost samples | 0 |
| HugePages | Backing verified | YES |

---

# Appendix: Compiler Attributes Reference

| Attribute | What it does | Where we use it |
|-----------|-------------|-----------------|
| `__attribute__((hot))` | Place in hot text section (I-cache) | All hot loop functions |
| `__attribute__((cold))` | Place in cold section | Error handlers |
| `__attribute__((always_inline))` | Force inline (ignore -O0) | Assembly wrappers |
| `__attribute__((flatten))` | Inline all callees recursively | CRTP batch functions |
| `__attribute__((noinline))` | Prevent inlining | Cold error paths |
| `alignas(64)` | Cache-line alignment | All hot data structures |
| `__builtin_expect(x, 1)` | Branch prediction hint | `IICPC_LIKELY` macro |
| `__builtin_prefetch(p, 0, 3)` | L1 prefetch for read | SoA array traversal |
| `__builtin_assume_aligned(p, N)` | Alignment guarantee | Auto-vectorization hints |
| `__restrict__` | No-alias pointer | `copy_cacheline` dst/src |

---

*This document describes a system that achieves 13.99 million transactions per second with sub-microsecond p99 latency on commodity hardware. Every design decision — from the memory allocator to the wire protocol to the CPU core map — exists to eliminate a measurable source of latency or non-determinism. Nothing is accidental.*
