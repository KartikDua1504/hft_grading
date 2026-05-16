#pragma once
// =============================================================================
// compiler_service.hpp — Compile Contestant C++ Code
// =============================================================================
// Takes contestant source code, compiles it with GCC inside an Alpine Docker
// container, produces a statically-linked musl binary.
//
// The binary runs inside Firecracker with ZERO external dependencies.
//
// Compilation flags (strict):
//   g++ -std=c++20 -O2 -march=x86-64-v3 -static -o contestant src.cpp
//   -I/sdk/include (our protocol headers)
//   -Wall -Werror (no warnings allowed)
//   Timeout: 30 seconds
// =============================================================================

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

namespace iicpc {

struct CompileConfig {
    const char* source_path    = nullptr;  // /path/to/contestant.cpp
    const char* output_path    = nullptr;  // /path/to/contestant (binary)
    const char* sdk_include    = nullptr;  // /path/to/sdk/include
    const char* docker_image   = "iicpc-compiler";  // Alpine + GCC image
    int         timeout_secs   = 30;
    bool        use_docker     = true;     // false = compile on host directly
};

struct CompileResult {
    bool     success     = false;
    int      exit_code   = -1;
    char     error[4096] = {};
    double   compile_secs = 0.0;
};

class CompilerService {
public:
    // =========================================================================
    // Compile contestant code
    // =========================================================================
    static CompileResult compile(const CompileConfig& cfg) noexcept {
        CompileResult result{};

        if (!cfg.source_path || !cfg.output_path) {
            std::snprintf(result.error, sizeof(result.error),
                         "source_path and output_path required");
            return result;
        }

        auto start = std::chrono::steady_clock::now();

        if (cfg.use_docker) {
            result = compile_docker(cfg);
        } else {
            result = compile_host(cfg);
        }

        auto end = std::chrono::steady_clock::now();
        result.compile_secs = std::chrono::duration<double>(end - start).count();

        return result;
    }

private:
    static CompileResult compile_host(const CompileConfig& cfg) noexcept {
        CompileResult result{};

        // Build command
        char cmd[2048];
        int n = std::snprintf(cmd, sizeof(cmd),
            "timeout %d g++ -std=c++20 -O2 -march=x86-64-v3 "
            "-static -Wall -Werror "
            "-o '%s' '%s'",
            cfg.timeout_secs, cfg.output_path, cfg.source_path);

        if (cfg.sdk_include) {
            std::snprintf(cmd + n, sizeof(cmd) - static_cast<std::size_t>(n),
                         " -I'%s'", cfg.sdk_include);
        }

        // Capture stderr
        char err_cmd[2200];
        std::snprintf(err_cmd, sizeof(err_cmd), "%s 2>&1", cmd);

        FILE* pipe = ::popen(err_cmd, "r");
        if (!pipe) {
            std::snprintf(result.error, sizeof(result.error),
                         "popen failed: %s", std::strerror(errno));
            return result;
        }

        std::size_t err_len = 0;
        char buf[256];
        while (::fgets(buf, sizeof(buf), pipe)) {
            std::size_t len = std::strlen(buf);
            if (err_len + len < sizeof(result.error) - 1) {
                std::memcpy(result.error + err_len, buf, len);
                err_len += len;
            }
        }
        result.error[err_len] = '\0';

        int status = ::pclose(pipe);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        result.success = (result.exit_code == 0);

        return result;
    }

    static CompileResult compile_docker(const CompileConfig& cfg) noexcept {
        CompileResult result{};

        // Docker run: mount source, SDK, and output directory
        char cmd[4096];
        std::snprintf(cmd, sizeof(cmd),
            "timeout %d docker run --rm "
            "-v '%s:/src/contestant.cpp:ro' "
            "-v '%s:/sdk/include:ro' "
            "-v '%s:/out' "
            "%s "
            "sh -c '"
            "g++ -std=c++20 -O2 -march=x86-64-v3 -static -Wall -Werror "
            "-I/sdk/include -o /out/contestant /src/contestant.cpp"
            "' 2>&1",
            cfg.timeout_secs,
            cfg.source_path,
            cfg.sdk_include ? cfg.sdk_include : "/dev/null",
            cfg.output_path, // Actually the output directory
            cfg.docker_image);

        FILE* pipe = ::popen(cmd, "r");
        if (!pipe) {
            std::snprintf(result.error, sizeof(result.error),
                         "popen failed: %s", std::strerror(errno));
            return result;
        }

        std::size_t err_len = 0;
        char buf[256];
        while (::fgets(buf, sizeof(buf), pipe)) {
            std::size_t len = std::strlen(buf);
            if (err_len + len < sizeof(result.error) - 1) {
                std::memcpy(result.error + err_len, buf, len);
                err_len += len;
            }
        }
        result.error[err_len] = '\0';

        int status = ::pclose(pipe);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        result.success = (result.exit_code == 0);

        return result;
    }
};

} // namespace iicpc
