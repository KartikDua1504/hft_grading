#pragma once
// =============================================================================
// metrics_publisher.hpp — Redpanda/Kafka Metrics Publisher
// =============================================================================
// Publishes benchmark telemetry snapshots to Redpanda for downstream
// consumption by QuestDB (persistence) and Redis (leaderboard).
//
// Uses librdkafka C API for minimal overhead. Batches messages to
// amortize produce overhead.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// We use a lightweight socket-based ILP publisher instead of librdkafka
// to avoid the heavy dependency. Redpanda speaks Kafka, but for
// direct QuestDB ingestion, ILP over TCP is simpler and faster.

namespace iicpc {

// =============================================================================
// QuestDB ILP (InfluxDB Line Protocol) Publisher
// =============================================================================
// Writes metrics directly to QuestDB via TCP line protocol.
// Format: measurement,tag=value field=value timestamp_ns
// Example: latency,contestant=bot1 p50=520i,p99=1160i,tps=9700000 1715250000000000000
// =============================================================================

class QuestDBPublisher {
public:
    QuestDBPublisher() noexcept = default;
    ~QuestDBPublisher() noexcept { disconnect(); }

    [[nodiscard]] bool connect(const char* host, uint16_t port) noexcept {
        // TCP connect to QuestDB ILP port (default 9009)
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            std::fprintf(stderr, "[questdb] Failed to create socket\n");
            return false;
        }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        if (::inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
            std::fprintf(stderr, "[questdb] Invalid address: %s\n", host);
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::fprintf(stderr, "[questdb] Failed to connect to %s:%u: %s\n",
                         host, port, std::strerror(errno));
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // TCP_NODELAY for immediate sends
        int flag = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        std::fprintf(stderr, "[questdb] Connected to %s:%u\n", host, port);
        return true;
    }

    void disconnect() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    /// Publish a latency snapshot as ILP line protocol
    bool publish_latency(const char* benchmark_id,
                         const char* contestant_id,
                         uint64_t timestamp_ns,
                         double tps,
                         uint64_t p50_ns,
                         uint64_t p90_ns,
                         uint64_t p99_ns,
                         uint64_t p999_ns,
                         uint64_t max_ns,
                         uint64_t total_samples,
                         uint64_t drops) noexcept {
        if (fd_ < 0) return false;

        // ILP format: measurement,tag=val field=val timestamp
        char line[1024];
        int len = std::snprintf(line, sizeof(line),
            "benchmark_metrics,"
            "benchmark_id=%s,"
            "contestant_id=%s "
            "tps=%.1f,"
            "p50_ns=%lui,"
            "p90_ns=%lui,"
            "p99_ns=%lui,"
            "p999_ns=%lui,"
            "max_ns=%lui,"
            "total_samples=%lui,"
            "drops=%lui "
            "%lu\n",
            benchmark_id,
            contestant_id,
            tps,
            p50_ns, p90_ns, p99_ns, p999_ns, max_ns,
            total_samples, drops,
            timestamp_ns);

        if (len <= 0 || len >= static_cast<int>(sizeof(line))) return false;

        ssize_t sent = ::send(fd_, line, static_cast<std::size_t>(len), MSG_NOSIGNAL);
        if (sent != len) {
            std::fprintf(stderr, "[questdb] Send failed: %s\n", std::strerror(errno));
            return false;
        }

        total_published_++;
        return true;
    }

    /// Publish a leaderboard update
    bool publish_leaderboard(const char* contestant_id,
                             double score,
                             double tps,
                             uint64_t p99_ns,
                             uint64_t drops) noexcept {
        if (fd_ < 0) return false;

        char line[512];
        int len = std::snprintf(line, sizeof(line),
            "leaderboard,"
            "contestant_id=%s "
            "score=%.2f,"
            "tps=%.1f,"
            "p99_ns=%lui,"
            "drops=%lui\n",
            contestant_id, score, tps, p99_ns, drops);

        if (len <= 0) return false;
        ssize_t sent = ::send(fd_, line, static_cast<std::size_t>(len), MSG_NOSIGNAL);
        return sent == len;
    }

    uint64_t total_published() const noexcept { return total_published_; }

private:
    int fd_ = -1;
    uint64_t total_published_ = 0;
};

// =============================================================================
// Redis Leaderboard Publisher
// =============================================================================
// Minimal RESP (Redis Serialization Protocol) client for sorted set updates.
// No dependency on hiredis — pure socket RESP commands.
// =============================================================================

class RedisPublisher {
public:
    RedisPublisher() noexcept = default;
    ~RedisPublisher() noexcept { disconnect(); }

    [[nodiscard]] bool connect(const char* host, uint16_t port) noexcept {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, host, &addr.sin_addr);

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::fprintf(stderr, "[redis] Failed to connect to %s:%u\n", host, port);
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        std::fprintf(stderr, "[redis] Connected to %s:%u\n", host, port);
        return true;
    }

    void disconnect() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    /// ZADD leaderboard <score> <contestant_id>
    bool zadd_leaderboard(const char* contestant_id, double score) noexcept {
        if (fd_ < 0) return false;

        // RESP protocol: *4\r\n$4\r\nZADD\r\n$11\r\nleaderboard\r\n$<score_len>\r\n<score>\r\n$<id_len>\r\n<id>\r\n
        char score_str[32];
        int score_len = std::snprintf(score_str, sizeof(score_str), "%.2f", score);

        int id_len = static_cast<int>(std::strlen(contestant_id));

        char cmd[512];
        int cmd_len = std::snprintf(cmd, sizeof(cmd),
            "*4\r\n$4\r\nZADD\r\n$11\r\nleaderboard\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
            score_len, score_str, id_len, contestant_id);

        ssize_t sent = ::send(fd_, cmd, static_cast<std::size_t>(cmd_len), MSG_NOSIGNAL);
        if (sent != cmd_len) return false;

        // Read response (we don't block — just drain)
        char resp[64];
        ::recv(fd_, resp, sizeof(resp), MSG_DONTWAIT);
        return true;
    }

    /// HSET contestant:<id> field value (for detailed stats)
    bool hset(const char* key, const char* field, const char* value) noexcept {
        if (fd_ < 0) return false;

        int key_len = static_cast<int>(std::strlen(key));
        int field_len = static_cast<int>(std::strlen(field));
        int value_len = static_cast<int>(std::strlen(value));

        char cmd[1024];
        int cmd_len = std::snprintf(cmd, sizeof(cmd),
            "*4\r\n$4\r\nHSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
            key_len, key, field_len, field, value_len, value);

        ssize_t sent = ::send(fd_, cmd, static_cast<std::size_t>(cmd_len), MSG_NOSIGNAL);
        if (sent != cmd_len) return false;

        char resp[64];
        ::recv(fd_, resp, sizeof(resp), MSG_DONTWAIT);
        return true;
    }

private:
    int fd_ = -1;
};

} // namespace iicpc
