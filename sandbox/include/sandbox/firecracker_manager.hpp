#pragma once
// =============================================================================
// firecracker_manager.hpp — Firecracker MicroVM Sandbox Manager
// =============================================================================
// Manages Firecracker microVM lifecycle via the Firecracker REST API
// (Unix domain socket). Each contestant runs inside an isolated microVM
// with configurable vCPUs, memory, and block devices.
//
// Why Firecracker over cgroups:
//   - Full kernel isolation (separate kernel instance per contestant)
//   - No shared kernel attack surface
//   - ~125ms boot time (practically instant)
//   - Memory and CPU hard limits enforced by hypervisor, not kernel scheduler
//   - Used by AWS Lambda and Fargate in production
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

namespace iicpc {

// =============================================================================
// MicroVM Configuration
// =============================================================================
struct MicroVMConfig {
    // Firecracker binary
    const char* firecracker_bin = "/usr/local/bin/firecracker";

    // Kernel + rootfs
    const char* kernel_image_path = nullptr;  // vmlinux
    const char* rootfs_path = nullptr;        // ext4 rootfs image

    // Resources
    uint32_t vcpu_count = 2;
    uint64_t mem_size_mib = 256;

    // Networking (optional)
    bool enable_network = false;
    const char* tap_device = nullptr;
    const char* guest_mac = "AA:FC:00:00:00:01";

    // Boot args
    const char* boot_args = "console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda rw";

    // Socket path for API
    const char* api_socket_path = "/tmp/firecracker.socket";

    // Jailer settings (optional, for production hardening)
    bool use_jailer = false;
    const char* jailer_bin = "/usr/local/bin/jailer";
    uint32_t jail_id = 0;
};

// =============================================================================
// MicroVM State
// =============================================================================
enum class VMState : uint8_t {
    CREATED,
    CONFIGURED,
    RUNNING,
    STOPPED,
    FAILED,
};

struct VMStatus {
    VMState state = VMState::CREATED;
    pid_t firecracker_pid = -1;
    int api_socket_fd = -1;
    char socket_path[256] = {};
    char error[256] = {};
};

// =============================================================================
// Firecracker Manager
// =============================================================================
class FirecrackerManager {
public:
    FirecrackerManager() noexcept = default;
    ~FirecrackerManager() noexcept { destroy(); }

    // =========================================================================
    // Create and start a microVM
    // =========================================================================
    [[nodiscard]] bool create(const MicroVMConfig& config) noexcept {
        config_ = config;

        // Generate unique socket path
        std::snprintf(status_.socket_path, sizeof(status_.socket_path),
                      "/tmp/firecracker_%u.sock", static_cast<unsigned>(getpid()));
        ::unlink(status_.socket_path);

        // Fork and exec firecracker
        pid_t pid = ::fork();
        if (pid < 0) {
            set_error("fork failed: %s", std::strerror(errno));
            return false;
        }

        if (pid == 0) {
            // Child: exec firecracker
            ::execl(config.firecracker_bin, "firecracker",
                    "--api-sock", status_.socket_path,
                    "--level", "Warning",
                    nullptr);
            // If exec fails
            ::_exit(127);
        }

        // Parent: wait for API socket to appear
        status_.firecracker_pid = pid;
        status_.state = VMState::CREATED;

        // Wait for the socket to be created (Firecracker takes ~50ms)
        for (int i = 0; i < 100; ++i) {
            if (::access(status_.socket_path, F_OK) == 0) {
                break;
            }
            ::usleep(10'000); // 10ms
        }

        if (::access(status_.socket_path, F_OK) != 0) {
            set_error("Firecracker socket not created after 1s");
            status_.state = VMState::FAILED;
            return false;
        }

        // Connect to the API socket
        status_.api_socket_fd = connect_api_socket(status_.socket_path);
        if (status_.api_socket_fd < 0) {
            set_error("Failed to connect to Firecracker API socket");
            status_.state = VMState::FAILED;
            return false;
        }

        std::fprintf(stderr, "[firecracker] Process started (pid=%d, socket=%s)\n",
                     pid, status_.socket_path);
        return true;
    }

    // =========================================================================
    // Configure the VM (kernel, rootfs, vCPUs, memory)
    // =========================================================================
    [[nodiscard]] bool configure() noexcept {
        if (status_.state != VMState::CREATED) {
            set_error("Cannot configure: VM not in CREATED state");
            return false;
        }

        if (!config_.kernel_image_path || !config_.rootfs_path) {
            set_error("kernel_image_path and rootfs_path are required");
            return false;
        }

        // Set boot source
        char boot_json[1024];
        std::snprintf(boot_json, sizeof(boot_json),
            R"({"kernel_image_path": "%s", "boot_args": "%s"})",
            config_.kernel_image_path, config_.boot_args);

        if (!api_put("/boot-source", boot_json)) {
            set_error("Failed to set boot source");
            return false;
        }

        // Set machine config (vCPUs, memory)
        char machine_json[512];
        std::snprintf(machine_json, sizeof(machine_json),
            R"({"vcpu_count": %u, "mem_size_mib": %lu})",
            config_.vcpu_count, config_.mem_size_mib);

        if (!api_put("/machine-config", machine_json)) {
            set_error("Failed to set machine config");
            return false;
        }

        // Set rootfs drive
        char drive_json[1024];
        std::snprintf(drive_json, sizeof(drive_json),
            R"({"drive_id": "rootfs", "path_on_host": "%s", "is_root_device": true, "is_read_only": false})",
            config_.rootfs_path);

        if (!api_put("/drives/rootfs", drive_json)) {
            set_error("Failed to set rootfs drive");
            return false;
        }

        // Optional: network interface
        if (config_.enable_network && config_.tap_device) {
            char net_json[512];
            std::snprintf(net_json, sizeof(net_json),
                R"({"iface_id": "eth0", "guest_mac": "%s", "host_dev_name": "%s"})",
                config_.guest_mac, config_.tap_device);

            if (!api_put("/network-interfaces/eth0", net_json)) {
                std::fprintf(stderr, "[firecracker] WARNING: Failed to set network\n");
                // Non-fatal
            }
        }

        status_.state = VMState::CONFIGURED;
        std::fprintf(stderr, "[firecracker] Configured: %u vCPUs, %lu MiB RAM\n",
                     config_.vcpu_count, config_.mem_size_mib);
        return true;
    }

    // =========================================================================
    // Start the VM (boot the guest kernel)
    // =========================================================================
    [[nodiscard]] bool start() noexcept {
        if (status_.state != VMState::CONFIGURED) {
            set_error("Cannot start: VM not configured");
            return false;
        }

        if (!api_put("/actions", R"({"action_type": "InstanceStart"})")) {
            set_error("Failed to start VM instance");
            return false;
        }

        status_.state = VMState::RUNNING;
        std::fprintf(stderr, "[firecracker] VM started\n");
        return true;
    }

    // =========================================================================
    // Stop the VM
    // =========================================================================
    void stop() noexcept {
        if (status_.state == VMState::RUNNING) {
            // Send InstanceHalt action
            api_put("/actions", R"({"action_type": "SendCtrlAltDel"})");
            status_.state = VMState::STOPPED;
        }
    }

    // =========================================================================
    // Destroy the VM and clean up
    // =========================================================================
    void destroy() noexcept {
        stop();

        if (status_.api_socket_fd >= 0) {
            ::close(status_.api_socket_fd);
            status_.api_socket_fd = -1;
        }

        if (status_.firecracker_pid > 0) {
            ::kill(status_.firecracker_pid, SIGTERM);
            int wstatus;
            ::waitpid(status_.firecracker_pid, &wstatus, WNOHANG);
            // Force kill if still alive
            ::usleep(100'000);
            ::kill(status_.firecracker_pid, SIGKILL);
            ::waitpid(status_.firecracker_pid, &wstatus, 0);
            status_.firecracker_pid = -1;
        }

        if (status_.socket_path[0]) {
            ::unlink(status_.socket_path);
            status_.socket_path[0] = 0;
        }
    }

    // Accessors
    [[nodiscard]] VMState state() const noexcept { return status_.state; }
    [[nodiscard]] pid_t pid() const noexcept { return status_.firecracker_pid; }
    [[nodiscard]] const char* last_error() const noexcept { return status_.error; }
    [[nodiscard]] const char* socket_path() const noexcept { return status_.socket_path; }

private:
    // =========================================================================
    // REST API helpers (Firecracker uses HTTP over Unix socket)
    // =========================================================================
    static int connect_api_socket(const char* path) noexcept {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    bool api_put(const char* endpoint, const char* json_body) noexcept {
        // Reconnect for each request (Firecracker may close connection)
        int fd = connect_api_socket(status_.socket_path);
        if (fd < 0) return false;

        char request[4096];
        int body_len = static_cast<int>(std::strlen(json_body));
        int req_len = std::snprintf(request, sizeof(request),
            "PUT %s HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Accept: application/json\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            endpoint, body_len, json_body);

        ssize_t sent = ::send(fd, request, static_cast<std::size_t>(req_len), 0);
        if (sent != req_len) {
            ::close(fd);
            return false;
        }

        // Read response
        char response[4096];
        ssize_t n = ::recv(fd, response, sizeof(response) - 1, 0);
        ::close(fd);

        if (n <= 0) return false;
        response[n] = '\0';

        // Check for 2xx status
        bool ok = (std::strstr(response, "HTTP/1.1 2") != nullptr);
        if (!ok) {
            std::fprintf(stderr, "[firecracker] API %s failed:\n%s\n", endpoint, response);
        }
        return ok;
    }

    // Overload for plain string (no format args) — avoids -Wformat-security
    void set_error(const char* msg) noexcept {
        std::snprintf(status_.error, sizeof(status_.error), "%s", msg);
        std::fprintf(stderr, "[firecracker] ERROR: %s\n", status_.error);
    }

    template<typename... Args>
    void set_error(const char* fmt, Args... args) noexcept {
        std::snprintf(status_.error, sizeof(status_.error), fmt, args...);
        std::fprintf(stderr, "[firecracker] ERROR: %s\n", status_.error);
    }

    MicroVMConfig config_{};
    VMStatus status_{};
};

} // namespace iicpc
