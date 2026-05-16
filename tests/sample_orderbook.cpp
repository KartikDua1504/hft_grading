// =============================================================================
// sample_orderbook.cpp — Sample Contestant Orderbook Implementation
// =============================================================================
// THIS IS WHAT CONTESTANTS WRITE. Their code:
//   1. Connects to the host gateway via Unix domain socket
//   2. Receives orders (OrderEntry, CancelRequest)
//   3. Processes them through their orderbook (price-time priority)
//   4. Sends back responses (ContestantResponse with fills/acks)
//
// This sample implements a basic LOB with std::map (intentionally naive).
// Good contestants will use SoA arrays, arena allocators, etc.
// =============================================================================

#include "sdk/protocol.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <vector>
#include <deque>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>

using namespace iicpc;

// =============================================================================
// Response struct (must match sandbox_bridge.hpp ContestantResponse layout)
// =============================================================================
struct alignas(64) Response {
    uint32_t order_id;
    MsgType  response_type;
    AckStatus ack_status;
    uint8_t  _pad0;
    uint8_t  num_fills;
    int64_t  fill_price;
    int32_t  fill_qty;
    int32_t  remaining_qty;
    uint64_t recv_tsc;
    uint64_t send_tsc;
    uint8_t  _pad1[16];
};
static_assert(sizeof(Response) == 64);

// =============================================================================
// Simple Order struct
// =============================================================================
struct Order {
    uint32_t client_order_id;
    int64_t  price;
    int32_t  remaining_qty;
    Side     side;
};

// =============================================================================
// Naive Orderbook (std::map — contestants should do better)
// =============================================================================
class SimpleOrderbook {
public:
    // Process limit order: match then rest
    void add_order(const OrderEntry& entry, int fd) {
        // Try matching first
        int32_t remaining = entry.quantity;

        if (entry.side == Side::BUY) {
            // Match against asks (lowest first)
            while (remaining > 0 && !asks_.empty()) {
                auto it = asks_.begin();
                if (entry.order_type == OrderType::LIMIT &&
                    it->first > entry.price) break;

                auto& queue = it->second;
                while (!queue.empty() && remaining > 0) {
                    Order& resting = queue.front();
                    int32_t fill_qty = std::min(remaining, resting.remaining_qty);

                    // Send fill for this match
                    send_fill(fd, entry.client_order_id, it->first,
                              fill_qty, remaining - fill_qty, entry.side);

                    remaining -= fill_qty;
                    resting.remaining_qty -= fill_qty;
                    if (resting.remaining_qty == 0) queue.pop_front();
                }
                if (queue.empty()) asks_.erase(it);
            }
        } else {
            // Match against bids (highest first)
            while (remaining > 0 && !bids_.empty()) {
                auto it = std::prev(bids_.end());
                if (entry.order_type == OrderType::LIMIT &&
                    it->first < entry.price) break;

                auto& queue = it->second;
                while (!queue.empty() && remaining > 0) {
                    Order& resting = queue.front();
                    int32_t fill_qty = std::min(remaining, resting.remaining_qty);

                    send_fill(fd, entry.client_order_id, it->first,
                              fill_qty, remaining - fill_qty, entry.side);

                    remaining -= fill_qty;
                    resting.remaining_qty -= fill_qty;
                    if (resting.remaining_qty == 0) queue.pop_front();
                }
                if (queue.empty()) bids_.erase(it);
            }
        }

        // Rest remaining in book (for LIMIT orders only)
        if (remaining > 0 && entry.order_type == OrderType::LIMIT) {
            Order o{entry.client_order_id, entry.price, remaining, entry.side};
            if (entry.side == Side::BUY) {
                bids_[entry.price].push_back(o);
            } else {
                asks_[entry.price].push_back(o);
            }
            send_ack(fd, entry.client_order_id, AckStatus::ACCEPTED, remaining);
        } else {
            send_ack(fd, entry.client_order_id, AckStatus::ACCEPTED, 0);
        }
    }

private:
    void send_fill(int fd, uint32_t oid, int64_t price, int32_t qty,
                   int32_t remaining, Side side) {
        Response resp{};
        resp.order_id = oid;
        resp.response_type = MsgType::FILL;
        resp.ack_status = AckStatus::ACCEPTED;
        resp.num_fills = 1;
        resp.fill_price = price;
        resp.fill_qty = qty;
        resp.remaining_qty = remaining;
        ::send(fd, &resp, sizeof(resp), MSG_NOSIGNAL);
    }

    void send_ack(int fd, uint32_t oid, AckStatus status, int32_t remaining) {
        Response resp{};
        resp.order_id = oid;
        resp.response_type = MsgType::ORDER_ACK;
        resp.ack_status = status;
        resp.remaining_qty = remaining;
        ::send(fd, &resp, sizeof(resp), MSG_NOSIGNAL);
    }

    std::map<int64_t, std::deque<Order>> bids_;  // price → FIFO queue
    std::map<int64_t, std::deque<Order>> asks_;
};

// =============================================================================
// Main — Connect to gateway, process orders
// =============================================================================
int main(int argc, char* argv[]) {
    ::signal(SIGPIPE, SIG_IGN);

    const char* socket_path = "/tmp/iicpc_contest.sock";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gateway") == 0 && i + 1 < argc)
            socket_path = argv[++i];
    }

    std::fprintf(stderr, "[contestant] Connecting to %s\n", socket_path);

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { std::perror("socket"); return 1; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        std::perror("connect");
        return 1;
    }

    std::fprintf(stderr, "[contestant] Connected. Processing orders...\n");

    SimpleOrderbook book;
    alignas(64) uint8_t buf[4096];
    uint64_t orders_processed = 0;

    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;

        // Process all complete messages in buffer
        size_t offset = 0;
        while (offset + 1 <= static_cast<size_t>(n)) {
            MsgType type = peek_msg_type(buf + offset);
            uint32_t msize = msg_size(type);
            if (msize == 0 || offset + msize > static_cast<size_t>(n)) break;

            switch (type) {
                case MsgType::ORDER_ENTRY: {
                    auto& order = *reinterpret_cast<OrderEntry*>(buf + offset);
                    book.add_order(order, fd);
                    orders_processed++;
                    break;
                }
                case MsgType::CANCEL_REQUEST: {
                    // Simple: just ack (full cancel impl left for contestants)
                    auto& cancel = *reinterpret_cast<CancelRequest*>(buf + offset);
                    Response resp{};
                    resp.order_id = cancel.client_order_id;
                    resp.response_type = MsgType::CANCEL_ACK;
                    resp.ack_status = AckStatus::ACCEPTED;
                    ::send(fd, &resp, sizeof(resp), MSG_NOSIGNAL);
                    orders_processed++;
                    break;
                }
                default:
                    break;
            }
            offset += msize;
        }

        if (orders_processed % 100000 == 0 && orders_processed > 0) {
            std::fprintf(stderr, "[contestant] Processed %lu orders\n",
                         orders_processed);
        }
    }

    std::fprintf(stderr, "[contestant] Disconnected. Total: %lu orders\n",
                 orders_processed);
    ::close(fd);
    return 0;
}
