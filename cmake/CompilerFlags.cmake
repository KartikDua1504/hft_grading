# IICPC Benchmarking Platform — Compiler Flags
# "We engineered performance."
#
# These flags are tuned for Alder Lake i7-12700H with GCC 15.2 / Clang 20.
# Every flag has a reason. Nothing is cargo-culted.

# --- Detect compiler ---
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(IICPC_IS_GCC TRUE)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(IICPC_IS_CLANG TRUE)
endif()

# --- Base flags for ALL targets ---
set(IICPC_COMMON_FLAGS
    -march=native          # Full Alder Lake ISA: AVX2, AVX-VNNI, etc.
    -mtune=native          # Schedule for hybrid P+E cores
    -Wall -Wextra -Wpedantic
    -Wconversion -Wshadow -Wdouble-promotion
    -fno-omit-frame-pointer  # Required for perf/flamegraphs — zero cost in practice
)

# --- Hot-path flags (core, loadgen, telemetry) ---
set(IICPC_HOTPATH_FLAGS
    ${IICPC_COMMON_FLAGS}
    -O3                    # Full optimization
    -flto=auto             # Link-time optimization across TUs
    -fno-exceptions        # Zero-cost? Not quite. We don't use them on hot path.
    -fno-rtti              # No virtual dispatch on hot path
    -fstrict-aliasing      # Enable type-based alias analysis for better codegen
    -ffast-math            # IEEE relaxation OK for latency percentile math
    -funroll-loops         # Help the compiler with tight loops
    -fprefetch-loop-arrays # Explicit prefetch generation for array traversals
    -fno-plt               # Eliminate PLT indirection for external calls
    -fno-semantic-interposition  # Allow inlining across shared library boundaries
    -mprefer-vector-width=256    # Prefer 256-bit AVX2 vectors for auto-vectorization
)

# GCC-specific optimizations
if(IICPC_IS_GCC)
    list(APPEND IICPC_HOTPATH_FLAGS
        -ftree-vectorize           # Auto-vectorize (redundant with -O3 but explicit)
        -fopt-info-vec-optimized   # Report what got vectorized (build log audit)
        -fipa-pta                  # Interprocedural pointer analysis for aliasing
        -fdevirtualize-at-ltrans   # Devirtualize calls discovered during LTO
    )
endif()

# Clang-specific optimizations
if(IICPC_IS_CLANG)
    list(APPEND IICPC_HOTPATH_FLAGS
        -Rpass=loop-vectorize      # Report vectorized loops
        -mllvm -polly              # Polyhedral optimizer for nested loops
        -fforce-emit-vtables       # Emit vtables in every TU for devirtualization
    )
endif()

# --- Debug/sanitizer flags ---
set(IICPC_DEBUG_FLAGS
    ${IICPC_COMMON_FLAGS}
    -O0 -g3
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
)

# --- Benchmark flags (hot-path + profiling friendly) ---
set(IICPC_BENCH_FLAGS
    ${IICPC_HOTPATH_FLAGS}
    -g1                    # Minimal debug info for perf annotation
)

# --- Apply flags helper ---
function(iicpc_target_hotpath target)
    target_compile_options(${target} PRIVATE ${IICPC_HOTPATH_FLAGS})
    target_link_options(${target} PRIVATE -flto=auto)
    # Hint: ensure 64-byte struct alignment is not broken by padding
    # Use static_assert in code, not compiler flags
endfunction()

function(iicpc_target_bench target)
    target_compile_options(${target} PRIVATE ${IICPC_BENCH_FLAGS})
    target_link_options(${target} PRIVATE -flto=auto)
endfunction()

function(iicpc_target_debug target)
    target_compile_options(${target} PRIVATE ${IICPC_DEBUG_FLAGS})
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
endfunction()

# Profile-Guided Optimization (PGO) Support
# PGO workflow:
#   1. Build with `iicpc_target_pgo_generate` (instruments binary)
#   2. Run benchmarks to collect profile data → generates .gcda files
#   3. Rebuild with `iicpc_target_pgo_use` (optimizes using profile data)
#
# Usage:
#   cmake .. -DIICPC_PGO_PHASE=generate   # Step 1
#   ./bench_shm --duration 10             # Step 2 (run representative workload)
#   cmake .. -DIICPC_PGO_PHASE=use        # Step 3
#
# Expected improvement: 10-20% throughput, 5-15% latency reduction.
# PGO enables: optimal branch layout, cold code sinking, I-cache optimization.

set(IICPC_PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo_profiles" CACHE PATH
    "Directory for PGO profile data")

function(iicpc_target_pgo_generate target)
    target_compile_options(${target} PRIVATE
        ${IICPC_HOTPATH_FLAGS}
        -fprofile-generate=${IICPC_PGO_PROFILE_DIR}
    )
    target_link_options(${target} PRIVATE
        -flto=auto
        -fprofile-generate=${IICPC_PGO_PROFILE_DIR}
    )
endfunction()

function(iicpc_target_pgo_use target)
    target_compile_options(${target} PRIVATE
        ${IICPC_HOTPATH_FLAGS}
        -fprofile-use=${IICPC_PGO_PROFILE_DIR}
        -fprofile-correction  # Handle missing profile data gracefully
    )
    target_link_options(${target} PRIVATE
        -flto=auto
        -fprofile-use=${IICPC_PGO_PROFILE_DIR}
    )
endfunction()

# Auto-apply PGO phase if set
if(DEFINED IICPC_PGO_PHASE)
    if(IICPC_PGO_PHASE STREQUAL "generate")
        message(STATUS "  PGO Phase:     GENERATE (instrumenting binaries)")
        file(MAKE_DIRECTORY ${IICPC_PGO_PROFILE_DIR})
    elseif(IICPC_PGO_PHASE STREQUAL "use")
        message(STATUS "  PGO Phase:     USE (optimizing with profiles from ${IICPC_PGO_PROFILE_DIR})")
    else()
        message(WARNING "  Unknown PGO phase: ${IICPC_PGO_PHASE} (expected: generate|use)")
    endif()
endif()
