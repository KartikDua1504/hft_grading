# IICPC SDK — Orderbook Engine Kit

## Overview

You are building a **limit order book matching engine**. The platform blasts
realistic exchange order flow at your engine, and you must:

1. **Process incoming orders** (limit, market, cancel)
2. **Match aggressively crossing orders** (generate fills)
3. **Maintain a resting book** (orders that don't immediately match)
4. **Send responses** (ORDER_ACK for accepted, FILL for matched, CANCEL_ACK for cancelled)

## Scoring

Your engine is scored on three axes:

| Weight | Metric | What It Measures |
|--------|--------|-----------------|
| 40% | **Correctness** | Do your fills match the reference shadow orderbook? |
| 30% | **Throughput** | How many orders/sec can you process? |
| 30% | **Latency** | How fast is your order-to-response time? |

**Formula**: `Score = 0.4 × correctness + 0.3 × throughput_score + 0.3 × latency_score`

## Files

- `include/sdk/strategy_sdk.hpp` — Interface you implement (`IStrategy`)
- `include/sdk/protocol.hpp` — Binary message structs (OrderEntry, Fill, etc.)
- `include/sdk/gateway_client.hpp` — Platform client (handles socket I/O)
- `src/strategy_main.cpp` — Main wrapper (links with your code)
- `examples/example_mm.cpp` — Reference orderbook engine

## Quick Start

```cpp
#include "sdk/strategy_sdk.hpp"
#include "sdk/protocol.hpp"

class MyEngine final : public iicpc::IStrategy {
    void on_order(const iicpc::OrderEntry& order,
                  iicpc::IResponseSender& sender) noexcept override {
        // Process the order, match against your book
        // Send FILL responses for matches
        // Send ORDER_ACK when done
    }

    void on_cancel(const iicpc::CancelRequest& cancel,
                   iicpc::IResponseSender& sender) noexcept override {
        // Look up and cancel the order
        // Send CANCEL_ACK response
    }
};

iicpc::IStrategy* iicpc::create_strategy() {
    static MyEngine engine;
    return &engine;
}
```

## Build & Test

```bash
g++ -std=c++23 -O2 -I include -o engine your_engine.cpp src/strategy_main.cpp -lpthread
```

## Tips

- **No heap allocation** in the hot path (use arrays, not std::map)
- **Price-time priority** matching (highest bid first, lowest ask first, FIFO within level)
- Process orders **as fast as possible** — every microsecond counts
- Study `examples/example_mm.cpp` for a reference implementation
