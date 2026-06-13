// --- Multi-threaded TCP Echo Exchange ---
// One thread per connection — eliminates server-side contention.
// This server is fast enough to NOT be the bottleneck,
// allowing the client transport (io_uring vs epoll) to be measured directly.

#include "core/types.hpp"
#include "loadgen/payload_gen.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>

using namespace iicpc;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_total_msgs{0};

static void signal_handler(int) { g_running = false; }

/// Per-connection worker: tight recv→ack loop with no epoll overhead
static void* connection_worker(void* arg) {
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));

    uint8_t recv_buf[4096];
    uint64_t local_msgs = 0;

    while (g_running) {
        ssize_t n = ::recv(fd, recv_buf, sizeof(recv_buf), 0); // blocking
        if (n <= 0) break;

        std::size_t offset = 0;
        while (offset + sizeof(BenchmarkMessage) <= static_cast<std::size_t>(n)) {
            const auto* msg = reinterpret_cast<const BenchmarkMessage*>(recv_buf + offset);

            if (msg->magic != 0x49494350) {
                offset++;
                continue;
            }

            AckMessage ack{};
            ack.magic   = 0x41434B00;
            ack.bot_id  = msg->bot_id;
            ack.seq_num = msg->seq_num;
            ack.tsc     = msg->tsc;

            ::send(fd, &ack, sizeof(ack), MSG_NOSIGNAL);
            local_msgs++;

            offset += sizeof(BenchmarkMessage) + msg->body_len;
        }
    }

    g_total_msgs += local_msgs;
    ::close(fd);
    return nullptr;
}

int main(int argc, char* argv[]) {
    uint16_t port = 9999;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
    }

    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);
    ::signal(SIGPIPE, SIG_IGN);

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { std::perror("socket"); return 1; }

    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind"); ::close(listen_fd); return 1;
    }
    if (::listen(listen_fd, 4096) < 0) {
        std::perror("listen"); ::close(listen_fd); return 1;
    }

    std::fprintf(stderr, "[mt_exchange] Listening on port %u (thread-per-connection)\n", port);

    while (g_running) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd,
            reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        int flag = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, connection_worker,
                       reinterpret_cast<void*>(static_cast<intptr_t>(client_fd)));
        pthread_attr_destroy(&attr);
    }

    std::fprintf(stderr, "\n[mt_exchange] Total messages: %lu\n", g_total_msgs.load());
    ::close(listen_fd);
    return 0;
}
