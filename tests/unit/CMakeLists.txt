add_executable(provider_unit_tests
    bread_inventory_test.cpp
    crumbs_session_test.cpp
    dcmt_adapter_test.cpp
    health_test.cpp
    inventory_test.cpp
    provider_config_test.cpp
    proto_smoke_test.cpp
    rlht_adapter_test.cpp
    startup_discovery_test.cpp
)

target_include_directories(provider_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(provider_unit_tests PRIVATE
    anolis_provider_bread_crumbs
    anolis_provider_bread_core
    GTest::gtest_main
)

anolis_provider_bread_apply_warnings(provider_unit_tests)

if(WIN32)
    target_compile_definitions(provider_unit_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

include(GoogleTest)
gtest_discover_tests(provider_unit_tests
    TEST_PREFIX provider.unit.
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "unit"
        TIMEOUT 30
)

add_executable(provider_shell_tests
    shell_test.cpp
)

target_include_directories(provider_shell_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(provider_shell_tests PRIVATE
    anolis_provider_bread_core
    GTest::gtest_main
)

target_compile_definitions(provider_shell_tests PRIVATE
    ANOLIS_PROVIDER_BREAD_TEST_EXE="$<TARGET_FILE:anolis-provider-bread>"
    ANOLIS_PROVIDER_BREAD_TEST_CONFIG="${CMAKE_SOURCE_DIR}/config/example.local.yaml"
)

anolis_provider_bread_apply_warnings(provider_shell_tests)

if(WIN32)
    target_compile_definitions(provider_shell_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

gtest_discover_tests(provider_shell_tests
    TEST_PREFIX provider.shell.
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "shell"
        TIMEOUT 60
)