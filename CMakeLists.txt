cmake_minimum_required(VERSION 3.20)

option(ANOLIS_PROVIDER_BREAD_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" OFF)

project(anolis_provider_bread VERSION 0.9.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CTest)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Warnings.cmake)

# ThreadSanitizer (TSAN) — data-race detection. bread is single-threaded today
# (inline-mutex crumbs::Session, synchronous i2c on the dispatch thread), so TSAN
# finds little now; the lane is forward-looking for the planned worker-thread
# i2c-serialization convergence. Deps are rebuilt with TSAN via x64-linux-tsan.
option(ENABLE_TSAN "Enable ThreadSanitizer for data race detection" OFF)

if(ENABLE_TSAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(NOT WIN32)
            add_compile_options(-fsanitize=thread -g -fno-omit-frame-pointer)
            add_link_options(-fsanitize=thread)
            message(STATUS "ThreadSanitizer (TSAN) enabled for data race detection")
        else()
            message(WARNING "ThreadSanitizer not supported on Windows (Linux/macOS only)")
        endif()
    else()
        message(WARNING "ThreadSanitizer requested but compiler not supported: ${CMAKE_CXX_COMPILER_ID}")
    endif()
# Fallback: detect TSAN triplet automatically.
elseif(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "tsan")
    if(NOT WIN32)
        message(STATUS "ThreadSanitizer auto-detected via triplet: ${VCPKG_TARGET_TRIPLET}")
        add_compile_options(-fsanitize=thread -g -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
endif()

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

include(FetchContent)
# The shared SDK owns the anolis-protocol pin + the `adpp_proto` re-export (and the
# `protocol.pb.h` shim), so bread no longer declares them — a second `adpp_proto`
# target / divergent pin would be a hard error. Consumed as source (pinned release
# tarball); local dev overrides with -DFETCHCONTENT_SOURCE_DIR_ANOLIS_PROVIDER_SDK=<path>.
FetchContent_Declare(
    anolis_provider_sdk
    URL https://github.com/anolishq/anolis-provider-sdk/releases/download/v0.4.0/anolis-provider-sdk-0.4.0-source.tar.gz
    URL_HASH SHA256=c4505cb5dd678c3fba51d7e3bdd1c35809a42edcad576b6a7bde873d034949cd
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
FetchContent_MakeAvailable(anolis_provider_sdk)

# CRUMBS: pinned release tarball via FetchContent by default; set CRUMBS_DIR to use a source-tree override.
if(DEFINED CRUMBS_DIR AND NOT CRUMBS_DIR STREQUAL "")
    get_filename_component(CRUMBS_DIR_ABS "${CRUMBS_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${CRUMBS_DIR_ABS}/CMakeLists.txt")
        message(FATAL_ERROR
            "CRUMBS not found at: ${CRUMBS_DIR_ABS}\n"
            "Set CRUMBS_DIR to the root of the CRUMBS repository.")
    endif()
    set(CRUMBS_ENABLE_LINUX_HAL OFF CACHE BOOL "" FORCE)
    set(CRUMBS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CRUMBS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CRUMBS_DIR_ABS}" "${CMAKE_BINARY_DIR}/crumbs_subbuild")
    if(NOT TARGET crumbs::crumbs)
        add_library(crumbs::crumbs ALIAS crumbs)
    endif()
else()
    FetchContent_Declare(
        crumbs
        URL https://github.com/feastorg/CRUMBS/releases/download/v0.16.0/crumbs-0.16.0-source.tar.gz
        URL_HASH SHA256=b32812c84b41a81f16001c864255770cb365b40daa5ba7f187843a250eab7fb5
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    set(CRUMBS_ENABLE_LINUX_HAL OFF CACHE BOOL "" FORCE)
    set(CRUMBS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CRUMBS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(crumbs)
endif()

# bread-crumbs-contracts: pinned release tarball via FetchContent by default; set BREAD_CONTRACTS_DIR to use a source-tree override.
if(DEFINED BREAD_CONTRACTS_DIR AND NOT BREAD_CONTRACTS_DIR STREQUAL "")
    get_filename_component(BREAD_CONTRACTS_DIR_ABS "${BREAD_CONTRACTS_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${BREAD_CONTRACTS_DIR_ABS}/CMakeLists.txt")
        message(FATAL_ERROR
            "bread-crumbs-contracts not found at: ${BREAD_CONTRACTS_DIR_ABS}\n"
            "Set BREAD_CONTRACTS_DIR to the root of the bread-crumbs-contracts repository.")
    endif()
    add_subdirectory("${BREAD_CONTRACTS_DIR_ABS}" "${CMAKE_BINARY_DIR}/bread_contracts_subbuild")
else()
    FetchContent_Declare(
        bread_crumbs_contracts
        URL https://github.com/feastorg/bread-crumbs-contracts/releases/download/v0.9.0/bread-crumbs-contracts-0.9.0-source.tar.gz
        URL_HASH SHA256=4d56063a13d12f1dea0d86ac70b4b9bf256e62e6dc6675864db3cbde100a3044
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    FetchContent_MakeAvailable(bread_crumbs_contracts)
endif()

add_library(anolis_provider_bread_bread_contracts INTERFACE)
target_link_libraries(anolis_provider_bread_bread_contracts
    INTERFACE
        bread::contracts
        crumbs::crumbs
)


add_library(anolis_provider_bread_support STATIC
    src/config/config_schema.cpp
    src/config/provider_config.cpp
    src/logging/logger.cpp
)
target_include_directories(anolis_provider_bread_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# The declare-once config toolkit (SDK): schema declaration + emitter +
# validator + typed extraction, one contract for --config-schema and
# --check-config.
target_link_libraries(anolis_provider_bread_support PUBLIC
    yaml-cpp::yaml-cpp
    anolis::provider_sdk_config)
target_compile_definitions(anolis_provider_bread_support PUBLIC
    ANOLIS_PROVIDER_BREAD_VERSION="${PROJECT_VERSION}"
)
anolis_provider_bread_apply_warnings(anolis_provider_bread_support)

# The CRUMBS codec (encode/decode/frame_length/scan) builds on all platforms
# (CRUMBS_ENABLE_LINUX_HAL is OFF everywhere), so CrumbsTransport + the canned bus
# are portable and back the mock path everywhere; only the real LinuxI2cBus is
# Linux-gated (in runtime_state.cpp).
add_library(anolis_provider_bread_crumbs STATIC
    src/crumbs/session.cpp
    src/crumbs/crumbs_transport.cpp
    src/crumbs/crumbs_canned_bus.cpp
)
target_include_directories(anolis_provider_bread_crumbs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(anolis_provider_bread_crumbs PUBLIC
    anolis_provider_bread_support
    anolis_provider_bread_bread_contracts
    crumbs::crumbs
    anolis::provider_sdk_i2c
)
target_compile_definitions(anolis_provider_bread_crumbs PUBLIC
    ANOLIS_PROVIDER_BREAD_VERSION="${PROJECT_VERSION}"
)
anolis_provider_bread_apply_warnings(anolis_provider_bread_crumbs)

add_library(anolis_provider_bread_core STATIC
    src/core/bread_provider_runtime.cpp
    src/core/host_check.cpp
    src/core/runtime_state.cpp
    src/core/startup.cpp
    src/devices/common/bread_compatibility.cpp
    src/devices/common/device_adapter.cpp
    src/devices/common/inventory.cpp
    src/devices/common/set_confirm.cpp
    src/devices/common/watchdog.cpp
    src/devices/common/watchdog_keepalive.cpp
    src/devices/rlht/rlht_adapter.cpp
    src/devices/dcmt/dcmt_adapter.cpp
)
target_include_directories(anolis_provider_bread_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(anolis_provider_bread_core
    PUBLIC
        anolis::provider_sdk
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
