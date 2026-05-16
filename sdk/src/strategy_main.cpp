// =============================================================================
// strategy_main.cpp — main() wrapper for contestant strategies
// =============================================================================
// Contestants link against this. It handles:
//   1. Connect to gateway socket
//   2. Instantiate contestant's strategy via create_strategy()
//   3. Run the event loop until session ends
//
// Compile: g++ -std=c++20 -O2 -static -o strategy contestant.cpp strategy_main.cpp
// =============================================================================

#include "sdk/gateway_client.hpp"
#include "sdk/strategy_sdk.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>

static volatile sig_atomic_t g_running = 1;
static void sig_handler(int) { g_running = 0; }

int main(int argc, char* argv[]) {
    ::signal(SIGINT, sig_handler);
    ::signal(SIGTERM, sig_handler);
    ::signal(SIGPIPE, SIG_IGN);

    // Default gateway socket path
    const char* socket_path = "/tmp/iicpc_gateway.sock";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gateway") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        }
    }

    std::fprintf(stderr, "[strategy] Connecting to gateway: %s\n", socket_path);

    // Create contestant strategy
    iicpc::IStrategy* strategy = iicpc::create_strategy();
    if (!strategy) {
        std::fprintf(stderr, "[strategy] FATAL: create_strategy() returned null\n");
        return 1;
    }

    // Connect to gateway
    iicpc::GatewayClient client;
    if (!client.connect(socket_path)) {
        std::fprintf(stderr, "[strategy] FATAL: could not connect to gateway\n");
        return 1;
    }

    std::fprintf(stderr, "[strategy] Connected. Waiting for session start...\n");

    // Run event loop — returns when session ends or connection drops
    client.run(*strategy);

    std::fprintf(stderr, "[strategy] Session ended. PnL: %ld (realized: %ld)\n",
                 client.total_pnl(), client.realized_pnl());

    return 0;
}
