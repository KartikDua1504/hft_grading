// exchange_local.cpp — Local Exchange Test Harness
// Runs the full exchange pipeline locally (no Firecracker) for testing:
//   1. Starts matching engine + gateway (host side)
//   2. Forks contestant strategy (connects via UDS)
//   3. Streams market data for configured duration
//   4. Processes orders + fills
//   5. Reports final PnL + leaderboard
//
// Usage:
//   ./exchange_local [--duration 30] [--ticks-per-sec 10000]
//                    [--strategy /path/to/strategy_binary]

#include "exchange/match_engine.hpp"
#include "exchange/gateway.hpp"
#include "sdk/protocol.hpp"
#include "core/arena.hpp"
#include "core/tsc.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace iicpc;

static std::atomic<bool> g_shutdown{false};
static void sig_handler(int) { g_shutdown.store(true); }

struct LocalConfig {
    const char* strategy_binary = nullptr;
    uint32_t duration_secs      = 30;
    uint32_t ticks_per_sec      = 1000;
    const char* socket_path     = "/tmp/iicpc_gateway.sock";
};

static LocalConfig parse_args(int argc, char* argv[]) {
    LocalConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            cfg.duration_secs = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--ticks-per-sec") == 0 && i + 1 < argc)
            cfg.ticks_per_sec = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--strategy") == 0 && i + 1 < argc)
            cfg.strategy_binary = argv[++i];
        else if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            cfg.socket_path = argv[++i];
    }
    return cfg;
}

int main(int argc, char* argv[]) {
    ::signal(SIGINT, sig_handler);
    ::signal(SIGTERM, sig_handler);
    ::signal(SIGPIPE, SIG_IGN);

    auto cfg = parse_args(argc, argv);

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "--- IICPC Local Exchange ---\n");

    std::fprintf(stderr, "  Matching Engine + Gateway + Strategy Test\n\n");

    // 1. Initialize arena + matching engine
    HugePageArena arena;
    arena.init(256ULL * 1024 * 1024);
    if (!arena.is_initialized()) {
        std::fprintf(stderr, "[exchange] FATAL: arena init failed\n");
        return 1;
    }

    MatchEngineConfig me_cfg{};
    me_cfg.match_duration_ms = cfg.duration_secs * 1000;
    me_cfg.tick_interval_us = 1000000 / cfg.ticks_per_sec;

    MatchEngine engine;
    if (!engine.init(arena, me_cfg)) {
        std::fprintf(stderr, "[exchange] FATAL: engine init failed\n");
        return 1;
    }

    uint32_t contestant_id = engine.register_contestant(me_cfg.initial_cash);
    std::fprintf(stderr, "[exchange] Contestant registered (id=%u)\n",
                 contestant_id);

    // 2. Start gateway
    GatewayConfig gw_cfg{};
    gw_cfg.socket_path = cfg.socket_path;
    gw_cfg.contestant_id = contestant_id;

    Gateway gateway;
    gateway.init(gw_cfg);

    // 3. Fork strategy process (if binary provided)
    pid_t strategy_pid = -1;
    if (cfg.strategy_binary) {
        // Start gateway listener BEFORE forking strategy
        // Use thread so strategy can connect while we wait
        std::thread accept_thread([&]() {
            if (!gateway.listen_and_accept()) {
                std::fprintf(stderr, "[exchange] Gateway accept failed\n");
                g_shutdown.store(true);
            }
        });

        // Small delay to ensure socket is listening
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        strategy_pid = ::fork();
        if (strategy_pid < 0) {
            std::fprintf(stderr, "[exchange] fork() failed\n");
            return 1;
        }
        if (strategy_pid == 0) {
            // Child: exec strategy binary
            ::execl(cfg.strategy_binary, cfg.strategy_binary,
                    "--gateway", cfg.socket_path, nullptr);
            std::fprintf(stderr, "[strategy] exec failed: %s\n",
                         std::strerror(errno));
            ::_exit(127);
        }

        std::fprintf(stderr, "[exchange] Strategy process started (pid=%d)\n",
                     strategy_pid);
        accept_thread.join();
    } else {
        // No strategy binary — run in echo/passive mode
        std::fprintf(stderr, "[exchange] No strategy binary. Running passive.\n");
    }

    if (g_shutdown.load()) {
        if (strategy_pid > 0) { ::kill(strategy_pid, SIGTERM); ::waitpid(strategy_pid, nullptr, 0); }
        return 1;
    }

    // 4. Send session start
    auto start_ts = std::chrono::system_clock::now();
    uint64_t start_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            start_ts.time_since_epoch()).count());

    if (gateway.is_connected()) {
        gateway.send_session_start(me_cfg, start_ns);
        std::fprintf(stderr, "[exchange] Session started\n");
    }

    // 5. Main loop — tick + process orders + forward fills
    const auto match_start = std::chrono::steady_clock::now();
    const auto match_end = match_start +
        std::chrono::milliseconds(me_cfg.match_duration_ms);

    uint64_t tick_count = 0;
    uint64_t orders_processed = 0;
    int report_interval = 0;

    const auto tick_interval = std::chrono::microseconds(me_cfg.tick_interval_us);
    auto next_tick = match_start;

    alignas(64) uint8_t recv_buf[256];

    while (!g_shutdown.load() && std::chrono::steady_clock::now() < match_end) {
        auto now = std::chrono::steady_clock::now();

        // Generate market data tick at configured rate
        if (now >= next_tick) {
            uint64_t ts_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());

            MarketUpdate update = engine.generate_tick(ts_ns);

            if (gateway.is_connected()) {
                gateway.send_market_data(update);
            }

            tick_count++;
            next_tick += tick_interval;

            // Don't let ticks accumulate if we're behind
            if (next_tick < now) next_tick = now + tick_interval;
        }

        // Receive orders from contestant (non-blocking)
        if (gateway.is_connected()) {
            MsgType type = gateway.recv_from_contestant(recv_buf, sizeof(recv_buf));
            uint64_t ts_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());

            switch (type) {
                case MsgType::ORDER_ENTRY: {
                    auto& order = *reinterpret_cast<OrderEntry*>(recv_buf);
                    engine.process_order(contestant_id, order, ts_ns);
                    orders_processed++;
                    break;
                }
                case MsgType::CANCEL_REQUEST: {
                    auto& cancel = *reinterpret_cast<CancelRequest*>(recv_buf);
                    engine.process_cancel(contestant_id, cancel, ts_ns);
                    break;
                }
                case MsgType::HEARTBEAT:
                    break;
                default:
                    break;
            }
        }

        // Forward outbound messages (fills/acks) to contestant
        OutboundMsg outmsg;
        while (engine.pop_outbound(outmsg)) {
            gateway.forward_outbound(outmsg);
        }

        // Periodic report
        auto elapsed = std::chrono::steady_clock::now() - match_start;
        int secs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        if (secs > report_interval) {
            report_interval = secs;
            int64_t mid = (engine.book().best_bid_price() +
                          engine.book().best_ask_price()) / 2;
            auto& cs = engine.contestants();
            std::fprintf(stderr,
                "[exchange] t=%ds | ticks=%lu | orders=%lu | fills=%u | "
                "pos=%d | PnL=%ld\n",
                secs, tick_count, orders_processed,
                cs.total_fills[contestant_id],
                cs.position[contestant_id],
                cs.total_pnl(contestant_id, mid));
        }
    }

    // 6. Session end
    uint64_t end_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    SessionEnd session_end = engine.make_session_end(contestant_id, end_ns);

    if (gateway.is_connected()) {
        gateway.send_session_end(session_end);
        // Give strategy time to process
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    gateway.shutdown();

    // Kill strategy process
    if (strategy_pid > 0) {
        ::kill(strategy_pid, SIGTERM);
        int wstatus;
        ::waitpid(strategy_pid, &wstatus, 0);
    }

    // 7. Final report
    auto& cs = engine.contestants();
    int64_t mid = engine.book().best_bid_price() > 0
        ? (engine.book().best_bid_price() + engine.book().best_ask_price()) / 2
        : 1000000;

    double pnl_dollars = static_cast<double>(cs.total_pnl(contestant_id, mid)) /
                         static_cast<double>(PRICE_MULTIPLIER);
    double realized_dollars = static_cast<double>(cs.realized_pnl[contestant_id]) /
                              static_cast<double>(PRICE_MULTIPLIER);

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "--- IICPC Local Exchange ---\n");
    std::fprintf(stderr, "\n--- Match Results ---\n");
    std::fprintf(stderr, "  Duration:     %u seconds                                   \n",
                 cfg.duration_secs);
    std::fprintf(stderr, "  Ticks:        %-44lu  \n", tick_count);
    std::fprintf(stderr, "  Orders:       %-44lu  \n", orders_processed);
    std::fprintf(stderr, "  Fills:        %-44u  \n",
                 cs.total_fills[contestant_id]);
    std::fprintf(stderr, "  Position:     %-44d  \n",
                 cs.position[contestant_id]);
    std::fprintf(stderr, "  Realized PnL: $%-43.2f \n", realized_dollars);
    std::fprintf(stderr, "  Total PnL:    $%-43.2f \n", pnl_dollars);

    return 0;
}
