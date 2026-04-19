cmake_minimum_required(VERSION 3.20)

option(ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE "Enable CRUMBS/Linux hardware integration" ${UNIX})
option(ANOLIS_PROVIDER_BREAD_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" OFF)

project(anolis_provider_bread VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CTest)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Warnings.cmake)

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_provider_triplet "${VCPKG_TARGET_TRIPLET}")
    if(NOT _provider_triplet OR _provider_triplet MATCHES "windows")
        if(NOT _provider_triplet MATCHES "mingw")
            message(FATAL_ERROR
                "Unsupported Windows toolchain/triplet combination:\n"
                "  compiler: ${CMAKE_CXX_COMPILER_ID}\n"
                "  VCPKG_TARGET_TRIPLET: '${_provider_triplet}'\n"
                "Use MSVC presets (`dev-windows-debug`, `dev-windows-release`),\n"
                "or select an explicit MinGW triplet (e.g. x64-mingw-dynamic)."
            )
        endif()
    endif()
endif()

find_package(Protobuf REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

if(NOT DEFINED ANOLIS_PROTOCOL_DIR)
    set(ANOLIS_PROTOCOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/anolis-protocol" CACHE PATH "Path to anolis-protocol repository")
endif()
if(NOT DEFINED CRUMBS_DIR)
    set(CRUMBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../CRUMBS" CACHE PATH "Path to CRUMBS repository")
endif()
if(NOT DEFINED BREAD_CONTRACTS_DIR)
    set(BREAD_CONTRACTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bread-crumbs-contracts" CACHE PATH "Path to bread-crumbs-contracts repository")
endif()

get_filename_component(ANOLIS_PROTOCOL_DIR_ABS "${ANOLIS_PROTOCOL_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(CRUMBS_DIR_ABS "${CRUMBS_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(BREAD_CONTRACTS_DIR_ABS "${BREAD_CONTRACTS_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(ADPP_PROTO_IMPORT_ROOT "${ANOLIS_PROTOCOL_DIR_ABS}/proto")
set(ADPP_PROTO_V1_DIR "${ADPP_PROTO_IMPORT_ROOT}/anolis/deviceprovider/v1")
set(ADPP_PROTO_ENVELOPE "${ADPP_PROTO_V1_DIR}/envelope.proto")

if(NOT EXISTS "${ADPP_PROTO_ENVELOPE}")
    message(FATAL_ERROR
        "ADPP proto not found at: ${ADPP_PROTO_ENVELOPE}\n"
        "Expected anolis-protocol at: ${ANOLIS_PROTOCOL_DIR_ABS}\n"
        "Run: git submodule update --init --recursive\n"
        "Or set custom path: cmake -DANOLIS_PROTOCOL_DIR=/path/to/anolis-protocol ..."
    )
endif()

file(GLOB ADPP_PROTOS "${ADPP_PROTO_V1_DIR}/*.proto")
if(NOT ADPP_PROTOS)
    message(FATAL_ERROR
        "No ADPP v1 proto files found under: ${ADPP_PROTO_V1_DIR}\n"
        "Expected split protocol layout from anolis-protocol PR #1."
    )
endif()

if(NOT EXISTS "${CRUMBS_DIR_ABS}/CMakeLists.txt")
    message(FATAL_ERROR
        "CRUMBS repository not found at: ${CRUMBS_DIR_ABS}\n"
        "Set CRUMBS_DIR or place the repo beside anolis-provider-bread."
    )
endif()

if(NOT EXISTS "${BREAD_CONTRACTS_DIR_ABS}/CMakeLists.txt")
    message(FATAL_ERROR
        "bread-crumbs-contracts repository not found at: ${BREAD_CONTRACTS_DIR_ABS}\n"
        "Set BREAD_CONTRACTS_DIR or place the repo beside anolis-provider-bread."
    )
endif()

set(CRUMBS_ENABLE_LINUX_HAL OFF CACHE BOOL "" FORCE)
set(CRUMBS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CRUMBS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)

if(ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE requires a Linux host build.\n"
            "Use a Linux hardware preset (dev-linux-hardware-debug, dev-linux-hardware-release)\n"
            "or configure with ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE=OFF on other platforms."
        )
    endif()

    if(NOT DEFINED LINUX_WIRE_DIR)
        set(LINUX_WIRE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../linux-wire" CACHE PATH "Path to linux-wire repository")
    endif()
    get_filename_component(LINUX_WIRE_DIR_ABS "${LINUX_WIRE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

    if(EXISTS "${LINUX_WIRE_DIR_ABS}/CMakeLists.txt")
        set(LINUX_WIRE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        add_subdirectory("${LINUX_WIRE_DIR_ABS}" "${CMAKE_BINARY_DIR}/linux_wire_subbuild")
        message(STATUS "Using linux-wire source tree: ${LINUX_WIRE_DIR_ABS}")
    else()
        message(STATUS
            "LINUX_WIRE_DIR not found at '${LINUX_WIRE_DIR_ABS}'. "
            "CRUMBS will rely on installed linux_wire package visibility if Linux HAL is enabled."
        )
    endif()

    set(CRUMBS_ENABLE_LINUX_HAL ON CACHE BOOL "" FORCE)
endif()

add_subdirectory("${CRUMBS_DIR_ABS}" "${CMAKE_BINARY_DIR}/crumbs_subbuild")
add_subdirectory("${BREAD_CONTRACTS_DIR_ABS}" "${CMAKE_BINARY_DIR}/bread_contracts_subbuild")

add_library(anolis_provider_bread_bread_contracts INTERFACE)
target_link_libraries(anolis_provider_bread_bread_contracts
    INTERFACE
        bread::contracts
        crumbs
)

add_library(adpp_proto OBJECT ${ADPP_PROTOS})
target_link_libraries(adpp_proto PUBLIC protobuf::libprotobuf)
target_include_directories(adpp_proto SYSTEM PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

protobuf_generate(
    TARGET adpp_proto
    LANGUAGE cpp
    IMPORT_DIRS "${ADPP_PROTO_IMPORT_ROOT}"
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
)

# Compatibility shim: keep existing includes of "protocol.pb.h" working after proto split.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/protocol.pb.h"
"#pragma once\n"
"#include \"anolis/deviceprovider/v1/call.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/envelope.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/handshake.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/health.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/inventory.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/readiness.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/status.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/telemetry.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/types.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/value.pb.h\"\n")

add_library(anolis_provider_bread_support STATIC
    src/config/provider_config.cpp
    src/logging/logger.cpp
)
target_include_directories(anolis_provider_bread_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(anolis_provider_bread_support PUBLIC yaml-cpp::yaml-cpp)
target_compile_definitions(anolis_provider_bread_support PUBLIC
    ANOLIS_PROVIDER_BREAD_VERSION="${PROJECT_VERSION}"
)
anolis_provider_bread_apply_warnings(anolis_provider_bread_support)

add_library(anolis_provider_bread_crumbs STATIC
    src/crumbs/session.cpp
)
target_include_directories(anolis_provider_bread_crumbs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(anolis_provider_bread_crumbs PUBLIC anolis_provider_bread_support)
target_compile_definitions(anolis_provider_bread_crumbs PUBLIC
    ANOLIS_PROVIDER_BREAD_VERSION="${PROJECT_VERSION}"
)
anolis_provider_bread_apply_warnings(anolis_provider_bread_crumbs)

if(ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE)
    target_sources(anolis_provider_bread_crumbs PRIVATE
        src/crumbs/linux_transport.cpp
    )
    target_link_libraries(anolis_provider_bread_crumbs PUBLIC crumbs)
    target_compile_definitions(anolis_provider_bread_crumbs PUBLIC
        ANOLIS_PROVIDER_BREAD_HAS_CRUMBS=1
    )
endif()

add_library(anolis_provider_bread_core STATIC
    src/core/handlers.cpp
    src/core/health.cpp
    src/core/runtime_state.cpp
    src/core/startup.cpp
    src/core/transport/framed_stdio.cpp
    src/devices/common/bread_compatibility.cpp
    src/devices/common/inventory.cpp
    src/devices/rlht/rlht_adapter.cpp
    src/devices/dcmt/dcmt_adapter.cpp
)
target_include_directories(anolis_provider_bread_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(anolis_provider_bread_core
    PUBLIC
        adpp_proto
        anolis_provider_bread_bread_contracts
        anolis_provider_bread_crumbs
        anolis_provider_bread_support
        protobuf::libprotobuf
)
target_compile_definitions(anolis_provider_bread_core PUBLIC
    ANOLIS_PROVIDER_BREAD_VERSION="${PROJECT_VERSION}"
)
anolis_provider_bread_apply_warnings(anolis_provider_bread_core)

add_executable(anolis-provider-bread src/core/main.cpp)
target_include_directories(anolis-provider-bread PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(anolis-provider-bread PRIVATE anolis_provider_bread_core)
target_compile_definitions(anolis-provider-bread PRIVATE
    ANOLIS_PROVIDER_BREAD_VERSION="${PROJECT_VERSION}"
)
anolis_provider_bread_apply_warnings(anolis-provider-bread)

if(WIN32)
    target_compile_definitions(anolis_provider_bread_support PUBLIC _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(anolis_provider_bread_crumbs PUBLIC _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(anolis_provider_bread_core PUBLIC _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(anolis-provider-bread PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests/unit)
endif()