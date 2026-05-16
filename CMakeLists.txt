cmake_minimum_required(VERSION 3.25)
project(iicpc_benchmark
    VERSION 0.1.0
    LANGUAGES CXX C
    DESCRIPTION "IICPC HFT-Grade Distributed Benchmarking Platform"
)

# ==============================================================================
# Global Settings
# ==============================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # No GNU extensions — pure ISO C++23
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # For clangd / IDE integration
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build type defaults to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Include our compiler flags
include(cmake/CompilerFlags.cmake)

# ==============================================================================
# Options
# ==============================================================================
option(IICPC_USE_URING    "Use io_uring for async I/O (requires liburing)" ON)
option(IICPC_USE_HUGEPAGES "Use 2MB huge pages for arena allocator"       ON)
option(IICPC_BUILD_BENCH  "Build benchmark/validation targets"            ON)
option(IICPC_SANITIZE     "Build with ASan+UBSan (debug only)"            OFF)

# ==============================================================================
# Third-party: liburing (build from source if not found)
# ==============================================================================
if(IICPC_USE_URING)
    find_library(LIBURING_LIB NAMES uring)
    find_path(LIBURING_INCLUDE NAMES liburing.h)

    if(LIBURING_LIB AND LIBURING_INCLUDE)
        message(STATUS "Found system liburing: ${LIBURING_LIB}")
        add_library(liburing INTERFACE)
        target_link_libraries(liburing INTERFACE ${LIBURING_LIB})
        target_include_directories(liburing INTERFACE ${LIBURING_INCLUDE})
    else()
        message(STATUS "liburing not found — building from source via FetchContent")
        include(FetchContent)
        FetchContent_Declare(
            liburing_src
            GIT_REPOSITORY https://github.com/axboe/liburing.git
            GIT_TAG        liburing-2.9
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(liburing_src)

        # liburing uses a Makefile, so we build it as an external project
        include(ExternalProject)
        ExternalProject_Add(liburing_build
            SOURCE_DIR ${liburing_src_SOURCE_DIR}
            CONFIGURE_COMMAND ${liburing_src_SOURCE_DIR}/configure
                --prefix=${CMAKE_BINARY_DIR}/liburing_install
            BUILD_COMMAND make -j${CMAKE_BUILD_PARALLEL_LEVEL}
                -C ${liburing_src_SOURCE_DIR}
            INSTALL_COMMAND make -C ${liburing_src_SOURCE_DIR}
                install PREFIX=${CMAKE_BINARY_DIR}/liburing_install
            BUILD_IN_SOURCE TRUE
        )

        add_library(liburing INTERFACE)
        add_dependencies(liburing liburing_build)
        target_include_directories(liburing INTERFACE
            ${CMAKE_BINARY_DIR}/liburing_install/include)
        target_link_directories(liburing INTERFACE
            ${CMAKE_BINARY_DIR}/liburing_install/lib)
        target_link_libraries(liburing INTERFACE uring)
    endif()
    add_compile_definitions(IICPC_HAS_URING=1)
else()
    add_compile_definitions(IICPC_HAS_URING=0)
endif()

# ==============================================================================
# Core Library (arena, ring buffer, flat map, HDR histogram, TSC)
# ==============================================================================
add_library(iicpc_core STATIC
    core/src/arena.cpp
    core/src/hdr_histogram.cpp
)
target_include_directories(iicpc_core PUBLIC core/include)
iicpc_target_hotpath(iicpc_core)
target_link_libraries(iicpc_core PUBLIC pthread)

# ==============================================================================
# Load Generator Library
# ==============================================================================
add_library(iicpc_loadgen STATIC
    loadgen/src/bot_fleet.cpp
    loadgen/src/payload_gen.cpp
    loadgen/src/io_engine.cpp
)
target_include_directories(iicpc_loadgen PUBLIC loadgen/include)
target_link_libraries(iicpc_loadgen PUBLIC iicpc_core)
if(IICPC_USE_URING)
    target_link_libraries(iicpc_loadgen PUBLIC liburing)
endif()
iicpc_target_hotpath(iicpc_loadgen)

# ==============================================================================
# Telemetry Consumer
# ==============================================================================
add_library(iicpc_telemetry STATIC
    telemetry/src/consumer.cpp
)
target_include_directories(iicpc_telemetry PUBLIC telemetry/include)
target_link_libraries(iicpc_telemetry PUBLIC iicpc_core)
iicpc_target_hotpath(iicpc_telemetry)

# ==============================================================================
# Dummy Exchange Server (Stage 1 validation target)
# ==============================================================================
add_executable(dummy_exchange server/src/dummy_exchange.cpp)
target_link_libraries(dummy_exchange PRIVATE iicpc_core iicpc_loadgen)
if(IICPC_USE_URING)
    target_link_libraries(dummy_exchange PRIVATE liburing)
endif()
iicpc_target_hotpath(dummy_exchange)

# ==============================================================================
# Benchmark / Validation Targets
# ==============================================================================
if(IICPC_BUILD_BENCH)
    # Full Checkpoint 1 validation harness
    add_executable(validate_checkpoint1 bench/validate_checkpoint1.cpp)
    target_link_libraries(validate_checkpoint1 PRIVATE
        iicpc_core iicpc_loadgen iicpc_telemetry)
    if(IICPC_USE_URING)
        target_link_libraries(validate_checkpoint1 PRIVATE liburing)
    endif()
    iicpc_target_bench(validate_checkpoint1)

    # Individual micro-benchmarks
    add_executable(bench_arena bench/bench_arena.cpp)
    target_link_libraries(bench_arena PRIVATE iicpc_core)
    iicpc_target_bench(bench_arena)

    add_executable(bench_ringbuf bench/bench_ringbuf.cpp)
    target_link_libraries(bench_ringbuf PRIVATE iicpc_core)
    iicpc_target_bench(bench_ringbuf)

    # Ultra-low-latency benchmark (Unix domain sockets, CRTP, zero-overhead)
    add_executable(bench_ultra bench/bench_ultra.cpp)
    target_link_libraries(bench_ultra PRIVATE
        iicpc_core iicpc_loadgen iicpc_telemetry)
    iicpc_target_bench(bench_ultra)

    # Pipeline multiplexed benchmark (writev scatter-gather)
    add_executable(bench_pipeline bench/bench_pipeline.cpp)
    target_link_libraries(bench_pipeline PRIVATE
        iicpc_core iicpc_loadgen iicpc_telemetry)
    iicpc_target_bench(bench_pipeline)

    # Shared-memory benchmark (zero syscalls, pure lock-free IPC)
    add_executable(bench_shm bench/bench_shm.cpp)
    target_link_libraries(bench_shm PRIVATE
        iicpc_core iicpc_loadgen iicpc_telemetry)
    iicpc_target_bench(bench_shm)

    # Order Blaster benchmark (firehose + spinlock + seqlock)
    add_executable(bench_blaster bench/bench_blaster.cpp)
    target_include_directories(bench_blaster PRIVATE sdk/include)
    target_link_libraries(bench_blaster PRIVATE iicpc_core iicpc_loadgen)
    iicpc_target_bench(bench_blaster)
endif()

# ==============================================================================
# Stage 2: gRPC Orchestrator + Sandbox
# ==============================================================================
option(IICPC_BUILD_STAGE2 "Build Stage 2 orchestrator and sandbox targets" ON)

if(IICPC_BUILD_STAGE2)
    # Find gRPC and Protobuf
    find_package(Protobuf REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPC REQUIRED grpc++ grpc)

    # Find grpc_cpp_plugin
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin
        HINTS /usr/bin /usr/local/bin)
    if(NOT GRPC_CPP_PLUGIN)
        message(WARNING "grpc_cpp_plugin not found — cannot generate gRPC stubs")
    endif()

    # Proto generation
    set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/orchestrator/proto/control.proto")
    set(PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
    file(MAKE_DIRECTORY ${PROTO_OUT})

    # Generate protobuf C++ files
    add_custom_command(
        OUTPUT
            "${PROTO_OUT}/control.pb.cc"
            "${PROTO_OUT}/control.pb.h"
            "${PROTO_OUT}/control.grpc.pb.cc"
            "${PROTO_OUT}/control.grpc.pb.h"
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --proto_path="${CMAKE_CURRENT_SOURCE_DIR}/orchestrator/proto"
            --cpp_out="${PROTO_OUT}"
            --grpc_out="${PROTO_OUT}"
            --plugin=protoc-gen-grpc="${GRPC_CPP_PLUGIN}"
            "${PROTO_SRC}"
        DEPENDS "${PROTO_SRC}"
        COMMENT "Generating gRPC/Protobuf C++ stubs"
    )

    # Proto library
    add_library(iicpc_proto STATIC
        "${PROTO_OUT}/control.pb.cc"
        "${PROTO_OUT}/control.grpc.pb.cc"
    )
    target_include_directories(iicpc_proto PUBLIC "${PROTO_OUT}")
    target_link_libraries(iicpc_proto PUBLIC
        protobuf::libprotobuf
        ${GRPC_LIBRARIES}
    )
    target_include_directories(iicpc_proto PUBLIC ${GRPC_INCLUDE_DIRS})
    # Suppress warnings in generated code
    target_compile_options(iicpc_proto PRIVATE -w)

    # Sandbox library (header-only for now)
    add_library(iicpc_sandbox INTERFACE)
    target_include_directories(iicpc_sandbox INTERFACE sandbox/include)

    # Orchestrator Server
    add_executable(orchestrator_server
        orchestrator/src/orchestrator_server.cpp
    )
    target_link_libraries(orchestrator_server PRIVATE
        iicpc_proto iicpc_sandbox)

    # Worker Agent
    add_executable(worker_agent
        orchestrator/src/worker_agent.cpp
    )
    target_link_libraries(worker_agent PRIVATE
        iicpc_proto iicpc_core iicpc_loadgen iicpc_telemetry iicpc_sandbox)

    # Pipeline E2E test (QuestDB + Redis publishing)
    add_executable(pipeline_e2e
        pipeline/src/pipeline_e2e.cpp
    )
    target_include_directories(pipeline_e2e PRIVATE pipeline/include)
    target_link_libraries(pipeline_e2e PRIVATE
        iicpc_core iicpc_loadgen iicpc_telemetry)

    # Integrated Worker (full production binary: gRPC + engine + sandbox + pipeline)
    add_executable(integrated_worker
        orchestrator/src/integrated_worker.cpp
    )
    target_include_directories(integrated_worker PRIVATE pipeline/include)
    target_link_libraries(integrated_worker PRIVATE
        iicpc_proto iicpc_core iicpc_loadgen iicpc_telemetry iicpc_sandbox)

    message(STATUS "  Stage 2:       ON (gRPC ${GRPC_VERSION}, Protobuf ${Protobuf_VERSION})")
else()
    message(STATUS "  Stage 2:       OFF")
endif()

# ==============================================================================
# Stage 3: Exchange + SDK (Competition Platform)
# ==============================================================================
message(STATUS "")
message(STATUS "--- Stage 3: Exchange + SDK ---")

# Exchange library (header-only: orderbook, matching engine, market data gen, gateway)
add_library(iicpc_exchange INTERFACE)
target_include_directories(iicpc_exchange INTERFACE
    exchange/include
    sdk/include
)
target_link_libraries(iicpc_exchange INTERFACE iicpc_core)

# SDK library (header-only: strategy interface, gateway client, protocol)
add_library(iicpc_sdk INTERFACE)
target_include_directories(iicpc_sdk INTERFACE sdk/include)

# Local exchange test harness
add_executable(exchange_local
    exchange/src/exchange_local.cpp
)
target_include_directories(exchange_local PRIVATE
    exchange/include sdk/include)
target_link_libraries(exchange_local PRIVATE iicpc_core)

# Example market-maker strategy (standalone binary)
add_executable(example_mm
    sdk/examples/example_mm.cpp
    sdk/src/strategy_main.cpp
)
target_include_directories(example_mm PRIVATE sdk/include)

message(STATUS "  Exchange:      ON (orderbook, matching engine, gateway)")
message(STATUS "  SDK:           ON (strategy interface, example MM)")

# Contest Runner (full E2E: compile → blast → validate → score)
add_executable(run_contest
    orchestrator/src/run_contest.cpp
)
target_include_directories(run_contest PRIVATE
    orchestrator/include exchange/include sandbox/include
    loadgen/include sdk/include)
target_link_libraries(run_contest PRIVATE iicpc_core)

# Sample contestant orderbook (for E2E testing)
add_executable(sample_orderbook
    tests/sample_orderbook.cpp
)
target_include_directories(sample_orderbook PRIVATE sdk/include)

message(STATUS "  Contest Runner: ON (compile → blast → validate → score)")

# ==============================================================================
# Summary
# ==============================================================================
message(STATUS "")
message(STATUS "=== IICPC Benchmark Platform ===")
message(STATUS "  C++ Standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler:      ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  io_uring:      ${IICPC_USE_URING}")
message(STATUS "  Huge Pages:    ${IICPC_USE_HUGEPAGES}")
message(STATUS "  Benchmarks:    ${IICPC_BUILD_BENCH}")
message(STATUS "  Sanitizers:    ${IICPC_SANITIZE}")
message(STATUS "================================")

