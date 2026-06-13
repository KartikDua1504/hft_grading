#pragma once
// metrics_publisher.hpp — Redpanda/Kafka Metrics Publisher
// Publishes benchmark telemetry snapshots to Redpanda for downstream
// consumption by QuestDB (persistence) and Redis (leaderboard).
//
// Uses librdkafka C API for minimal overhead. Batches messages to
// amortize produce overhead.

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

// QuestDB ILP (InfluxDB Line Protocol) Publisher
// Writes metrics directly to QuestDB via TCP line protocol.
// Format: measurement,tag=value field=value timestamp_ns
// Example: latency,contestant=bot1 p50=520i,p99=1160i,tps=9700000 1715250000000000000

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

// Redis Leaderboard Publisher
// Minimal RESP (Redis Serialization Protocol) client for sorted set updates.
// No dependency on hiredis — pure socket RESP commands.

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

// Redpanda/Kafka Publisher (Zero-Dependency)
// Speaks the Kafka produce protocol (ApiKey=0, ApiVersion=0) directly over TCP.
// No librdkafka dependency — just raw socket + Kafka wire format.
// Publishes contest results to the `iicpc.results` topic for downstream
// consumption (leaderboard updates, audit log, event replay).

class RedpandaPublisher {
public:
    RedpandaPublisher() noexcept = default;
    ~RedpandaPublisher() noexcept { disconnect(); }

    [[nodiscard]] bool connect(const char* host, uint16_t port) noexcept {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        if (::inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
            ::close(fd_); fd_ = -1;
            return false;
        }

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::fprintf(stderr, "[redpanda] Failed to connect to %s:%u: %s\n",
                         host, port, std::strerror(errno));
            ::close(fd_); fd_ = -1;
            return false;
        }

        int flag = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        std::fprintf(stderr, "[redpanda] Connected to %s:%u\n", host, port);
        return true;
    }

    void disconnect() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    /// Publish a contest result as JSON to the iicpc.results topic.
    /// Format: {"contestant":"<id>","score":<s>,"tps":<t>,"p99_ns":<p>,"drops":<d>,"ts":<ts>}
    bool publish_result(const char* contestant_id,
                        double score,
                        double tps,
                        uint64_t p99_ns,
                        uint64_t drops) noexcept {
        if (fd_ < 0) return false;

        // Build JSON payload
        char json[512];
        int json_len = std::snprintf(json, sizeof(json),
            "{\"contestant\":\"%s\",\"score\":%.4f,\"tps\":%.1f,"
            "\"p99_ns\":%lu,\"drops\":%lu,\"ts\":%lu}",
            contestant_id, score, tps, p99_ns, drops, now_ms());

        if (json_len <= 0 || json_len >= static_cast<int>(sizeof(json))) return false;

        // ── Kafka Produce Request (ApiKey=0, ApiVersion=0) ──
        // Wire format (big-endian):
        //   [4] request_size (total bytes after this field)
        //   [2] api_key = 0 (Produce)
        //   [2] api_version = 0
        //   [4] correlation_id
        //   [2] client_id length + [N] client_id
        //   [2] required_acks = 1
        //   [4] timeout_ms = 5000
        //   [4] topic_count = 1
        //   [2] topic_name length + [N] topic_name
        //   [4] partition_count = 1
        //   [4] partition = 0
        //   [4] message_set_size
        //   [message_set]:
        //     [8] offset = 0
        //     [4] message_size
        //     [message]:
        //       [4] crc32
        //       [1] magic = 0
        //       [1] attributes = 0
        //       [4] key_length = -1 (null)
        //       [4] value_length + [N] value (our JSON)

        const char* topic = "iicpc.results";
        int topic_len = static_cast<int>(std::strlen(topic));
        const char* client = "iicpc-engine";
        int client_len = static_cast<int>(std::strlen(client));

        // Pre-calculate sizes
        int msg_payload = 4 + 1 + 1 + 4 + 4 + json_len; // crc + magic + attrs + key(-1) + value
        int msg_size = msg_payload;
        int msgset_size = 8 + 4 + msg_size; // offset + size + message
        int body_size = 2 + 2 + 4 + (2 + client_len) + 2 + 4 +
                        4 + (2 + topic_len) + 4 + 4 + 4 + msgset_size;

        // Build the request
        uint8_t buf[2048];
        int pos = 0;

        auto put_i32 = [&](int32_t v) {
            buf[pos++] = (v >> 24) & 0xFF;
            buf[pos++] = (v >> 16) & 0xFF;
            buf[pos++] = (v >> 8) & 0xFF;
            buf[pos++] = v & 0xFF;
        };
        auto put_i16 = [&](int16_t v) {
            buf[pos++] = (v >> 8) & 0xFF;
            buf[pos++] = v & 0xFF;
        };
        auto put_i64 = [&](int64_t v) {
            for (int i = 7; i >= 0; --i) buf[pos++] = (v >> (i*8)) & 0xFF;
        };
        auto put_i8 = [&](int8_t v) { buf[pos++] = v; };
        auto put_str = [&](const char* s, int len) {
            put_i16(static_cast<int16_t>(len));
            std::memcpy(buf + pos, s, len); pos += len;
        };
        auto put_bytes = [&](const char* s, int len) {
            put_i32(len);
            std::memcpy(buf + pos, s, len); pos += len;
        };

        // Request envelope
        put_i32(body_size);                    // request_size
        put_i16(0);                            // api_key = Produce
        put_i16(0);                            // api_version = 0
        put_i32(static_cast<int32_t>(++correlation_id_)); // correlation_id
        put_str(client, client_len);           // client_id

        // Produce request body
        put_i16(1);                            // required_acks
        put_i32(5000);                         // timeout_ms
        put_i32(1);                            // topic_count
        put_str(topic, topic_len);             // topic name
        put_i32(1);                            // partition_count
        put_i32(0);                            // partition

        // MessageSet
        put_i32(msgset_size);                  // message_set_size
        put_i64(0);                            // offset
        put_i32(msg_size);                     // message_size

        // Message (skip CRC for now — Redpanda in dev mode doesn't enforce it)
        int crc_pos = pos;
        put_i32(0);                            // crc32 placeholder
        put_i8(0);                             // magic
        put_i8(0);                             // attributes
        put_i32(-1);                           // key = null
        put_bytes(json, json_len);             // value = JSON

        // Calculate CRC32 over bytes after the CRC field
        uint32_t crc = crc32(buf + crc_pos + 4, pos - crc_pos - 4);
        buf[crc_pos]     = (crc >> 24) & 0xFF;
        buf[crc_pos + 1] = (crc >> 16) & 0xFF;
        buf[crc_pos + 2] = (crc >> 8) & 0xFF;
        buf[crc_pos + 3] = crc & 0xFF;

        // Send
        ssize_t sent = ::send(fd_, buf, pos, MSG_NOSIGNAL);
        if (sent != pos) {
            std::fprintf(stderr, "[redpanda] Send failed: %s\n", std::strerror(errno));
            return false;
        }

        // Drain response (don't block)
        uint8_t resp[256];
        ::recv(fd_, resp, sizeof(resp), MSG_DONTWAIT);

        total_published_++;
        return true;
    }

    uint64_t total_published() const noexcept { return total_published_; }

private:
    int fd_ = -1;
    uint32_t correlation_id_ = 0;
    uint64_t total_published_ = 0;

    static uint64_t now_ms() noexcept {
        struct timespec ts;
        ::clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    }

    /// CRC32 (IEEE 802.3 polynomial — same as Kafka uses)
    static uint32_t crc32(const uint8_t* data, size_t len) noexcept {
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < len; ++i) {
            crc ^= data[i];
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
            }
        }
        return crc ^ 0xFFFFFFFF;
    }
};

} // namespace iicpc
