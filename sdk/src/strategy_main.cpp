// --- main() wrapper for contestant engines ---
// Handles socket connection and event loop.
// Contestants link against this and implement create_strategy().

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

    const char* socket_path = "/tmp/iicpc_contest.sock";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gateway") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        }
    }

    std::fprintf(stderr, "[engine] Connecting to platform: %s\n", socket_path);

    iicpc::IStrategy* engine = iicpc::create_strategy();
    if (!engine) {
        std::fprintf(stderr, "[engine] FATAL: create_strategy() returned null\n");
        return 1;
    }

    iicpc::GatewayClient client;
    if (!client.connect(socket_path)) {
        std::fprintf(stderr, "[engine] FATAL: could not connect to platform\n");
        return 1;
    }

    std::fprintf(stderr, "[engine] Connected. Waiting for orders...\n");

    // Run event loop — returns when session ends or connection drops
    client.run(*engine);

    return 0;
}
