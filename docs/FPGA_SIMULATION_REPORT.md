# FPGA Simulation Report

**Generated:** 2026-06-06T08:20:00+05:30  
**Verilator:** 5.046 (2026-02-28)  
**Platform:** Intel i7-12700H, Gentoo Linux 6.12, GCC 15.2  
**Architecture:** 2-Stage Pipeline with BBO Forwarding (True II=1)

---

## Sequencer Core (`sequencer_core.sv`)

**Configuration:** 4 ports, 256-entry DMA ring, 250 MHz clock (4ns period)

### Test Results

| Test | Description | Status |
|------|------------|--------|
| **Test 1** | Single port, 100 sequential orders | ✅ PASS |
| **Test 2** | All 4 ports simultaneous (fairness) | ✅ PASS |
| **Test 3** | Cancel requests (msg_type=11) | ✅ PASS |
| **Test 4** | Backpressure (consumer stalls) | ✅ PASS |

### Summary
```
Total sequenced:  610
Total drops:      20
Total received:   610
Last seq_no:      610
Errors:           0
*** ALL TESTS PASSED ***
```

---

## Matching Engine (`match_engine_fpga.sv`) — True II=1 Pipeline

**Configuration:** 1024 max orders, 256 max levels, 250 MHz clock  
**Architecture:** 2-stage pipeline with combinational BBO forwarding

### Pipeline Design

```
           ┌───────────────────────┐    ┌─────────────────────────────┐
in_valid──►│ STAGE 1: DECODE       │───►│ STAGE 2: EXECUTE + EMIT     │
in_ready◄──│ • Latch input         │    │ • Insert / Match / Cancel   │──► fill/ack
           │ • Cross-detect (comb) │    │ • BBO update                │
           │ • Cancel hash         │    │ • Drive outputs             │
           │ (1 cycle)             │    │ (1 cycle insert/cancel)     │
           └───────────────────────┘    │ (N cycles crossing sweep)   │
                     ▲                  └─────────────┬───────────────┘
                     │      BBO Forwarding            │
                     └────────────────────────────────┘

  Key: in_ready stays HIGH during 1-cycle Execute (insert/cancel),
       allowing back-to-back acceptance every clock cycle (II=1).
       Only multi-cycle crossing sweeps cause pipeline stalls.
```

### Test Results

| Test | Description | Status |
|------|------------|--------|
| **Test 1** | Insert limit BUY (no match, ack ACCEPTED) | ✅ PASS |
| **Test 2** | Insert matching SELL (fill generated) | ✅ PASS |
| **Test 3** | Build 5 buy + 5 sell levels | ✅ PASS |
| **Test 4** | Cancel order | ✅ PASS |
| **Test 5** | Aggressive crossing order (fill) | ✅ PASS |
| **Test 6** | Sequential throughput (1000 orders, blocking) | ✅ PASS |
| **Test 7** | Pipelined throughput (1000 non-crossing, II=1) | ✅ PASS |
| **Test 8** | Sustained crossing benchmark (1000 alternating) | ✅ PASS |

### Invariants Verified
- ✅ **II=1 achieved** — 0 stalls during 1000 non-crossing pipelined orders
- ✅ **BBO forwarding** — Consecutive inserts see correct BBO via combinational bypass
- ✅ **Price-time matching** — Fill generated at correct price when SELL crosses BUY
- ✅ **Book depth** — 10 resting orders after building 5 bid + 5 ask levels
- ✅ **Cancel processing** — stat_total_cancels incremented correctly
- ✅ **Crossing fills** — Aggressive order correctly matches resting counterparty
- ✅ **HW/TB fill agreement** — Hardware fill count (1000) matches testbench count (1000)

### Performance Benchmarks

#### Test 7: Pipelined Throughput (Non-Crossing, II=1)
```
Elapsed:    4,060 ns (4 µs)
Orders:     1,000 (non-crossing, pipelined)
Acks:       1,000
Stalls:     0 (verified II=1)
II:         1 cycle/order (4 ns)
Throughput: 246.3 M orders/sec (at 250 MHz)

=== Multi-Frequency Projection ===
@ 250 MHz: 246.3 M orders/sec (98.5% of physical limit)
@ 322 MHz: 317.2 M orders/sec (10G Ethernet native)
@ 500 MHz: 492.6 M orders/sec (Versal fast path)
```

#### Test 8: Sustained Crossing Throughput
```
Elapsed:    10,088 ns (10 µs)
Orders:     1,000 (crossing, pipelined)
Fills:      499
Throughput: 99.1 M orders/sec (sustained crossing, at 250 MHz)
```

#### Test 6: Sequential Throughput (Blocking Submit)
```
Elapsed:    28,000 ns (28 µs)
Orders:     1,000
Fills:      499
Acks:       1,000
Latency:    ~28 ns/order
Throughput: 35.7 M orders/sec (sequential/blocking, at 250 MHz)
```

### Architecture Evolution

| Metric | v1 (Blocking FSM) | v2 (3-Stage Pipeline) | v3 (II=1 Pipeline) |
|--------|-------------------|----------------------|-------------------|
| Architecture | 9-state blocking FSM | 3-stage feed-forward | 2-stage + BBO forwarding |
| Non-crossing II | ~6 cycles | 3 cycles | **1 cycle** |
| Pipelined throughput | N/A | 82.9M orders/sec | **246.3M orders/sec** |
| Sustained crossing | N/A | 70.9M orders/sec | **99.1M orders/sec** |
| % of physical limit | ~16% | 33% | **98.5%** |
| @ 500 MHz | N/A | 165.9M | **492.6M orders/sec** |
| Fill correctness | ❌ (sweep bug) | ✅ 1002/1002 | ✅ **1000/1000** |
| Stalls (non-crossing) | Always | Always | **0** |

### Summary
```
========================================
     MATCHING ENGINE RESULTS (II=1)
========================================
  Total orders:   3,013
  Total fills:    1,000 (HW) / 1,000 (TB)
  Total cancels:  1
  Resting orders: 1,012
  Total acks:     3,014
  Errors:         0
========================================
  *** ALL TESTS PASSED ***
```

---

## VCD Files

| File | Size | Duration |
|------|------|----------|
| `fpga/build/ver_seq/dump.vcd` | 1.2 MB | 5 µs |
| `fpga/build/ver_match/dump.vcd` | ~2 MB | 43 µs |

Open in GTKWave:
```bash
gtkwave fpga/build/ver_seq/dump.vcd fpga/build/ver_seq/sequencer.gtkw
gtkwave fpga/build/ver_match/dump.vcd fpga/build/ver_match/match_engine.gtkw
```
