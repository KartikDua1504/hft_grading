// dummy_exchange.cpp — Simple TCP Echo Exchange for Stage 1 Validation
// Accepts connections, reads BenchmarkMessages, echoes AckMessages.
// Uses epoll for concurrent connection handling on a single thread.
// Pinned to a dedicated core for consistent latency measurement.

#include "core/types.hpp"
#include "loadgen/payload_gen.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace iicpc;

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int) { g_running = 0; }

static bool set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int main(int argc, char* argv[]) {
    uint16_t port = 9999;
    int cpu_pin = -1;

    // Parse args
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
            cpu_pin = std::atoi(argv[++i]);
        }
    }

    // Pin to CPU
    if (cpu_pin >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_pin, &cpuset);
        ::sched_setaffinity(0, sizeof(cpuset), &cpuset);
        std::fprintf(stderr, "[exchange] Pinned to CPU %d\n", cpu_pin);
    }

    // Signal handling
    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);
    ::signal(SIGPIPE, SIG_IGN);

    // Create listening socket
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::perror("[exchange] socket");
        return 1;
    }

    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("[exchange] bind");
        ::close(listen_fd);
        return 1;
    }

    if (::listen(listen_fd, 4096) < 0) {
        std::perror("[exchange] listen");
        ::close(listen_fd);
        return 1;
    }

    set_nonblocking(listen_fd);

    // Create epoll
    int epoll_fd = ::epoll_create1(0);
    if (epoll_fd < 0) {
        std::perror("[exchange] epoll_create1");
        ::close(listen_fd);
        return 1;
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    std::fprintf(stderr, "[exchange] Listening on port %u\n", port);

    constexpr int MAX_EVENTS = 1024;
    struct epoll_event events[MAX_EVENTS];
    uint64_t total_msgs = 0;

    // Per-connection receive buffers (stack-allocated, good enough for dummy)
    // Real exchange would use arena. This is just a test harness.
    uint8_t recv_buf[sizeof(BenchmarkMessage) + 256];

    while (g_running) {
        int nfds = ::epoll_wait(epoll_fd, events, MAX_EVENTS, 100);

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == listen_fd) {
                // Accept new connections
                while (true) {
                    struct sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = ::accept4(listen_fd,
                        reinterpret_cast<struct sockaddr*>(&client_addr),
                        &client_len, SOCK_NONBLOCK);

                    if (client_fd < 0) break;

                    int flag = 1;
                    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                    struct epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLET;
                    cev.data.fd = client_fd;
                    ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev);
                }
            } else {
                // Read data from client
                int fd = events[i].data.fd;

                while (true) {
                    ssize_t n = ::recv(fd, recv_buf, sizeof(recv_buf), MSG_DONTWAIT);
                    if (n <= 0) {
                        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                            ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                            ::close(fd);
                        }
                        break;
                    }

                    // Parse message(s) from buffer
                    std::size_t offset = 0;
                    while (offset + sizeof(BenchmarkMessage) <= static_cast<std::size_t>(n)) {
                        const auto* msg = reinterpret_cast<const BenchmarkMessage*>(
                            recv_buf + offset);

                        if (msg->magic != 0x49494350) {
                            offset++;
                            continue;
                        }

                        // Build ACK
                        AckMessage ack{};
                        ack.magic   = 0x41434B00;
                        ack.bot_id  = msg->bot_id;
                        ack.seq_num = msg->seq_num;
                        ack.tsc     = msg->tsc;

                        // Send ACK (best-effort, non-blocking)
                        ::send(fd, &ack, sizeof(ack), MSG_NOSIGNAL | MSG_DONTWAIT);
                        total_msgs++;

                        offset += sizeof(BenchmarkMessage) + msg->body_len;
                    }
                }
            }
        }
    }

    std::fprintf(stderr, "\n[exchange] Shutting down. Total messages processed: %lu\n", total_msgs);
    ::close(epoll_fd);
    ::close(listen_fd);
    return 0;
}
