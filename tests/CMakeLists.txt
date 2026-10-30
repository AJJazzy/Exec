# Test executables, registered with CTest and labelled unit / integration / performance.
# Tests whose names contain MOCK or SIMULATED use simulated components (see docs/testing.md).

add_library(nexus_test_support STATIC support/test_main.cpp support/fixtures.cpp)
target_include_directories(nexus_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(nexus_test_support PUBLIC nexus_core)
nexus_configure_target(nexus_test_support)

function(nexus_add_test name label)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE nexus_test_support nexus_host)
    nexus_configure_target(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT 600)
endfunction()

nexus_add_test(test_core unit unit/test_core.cpp)
nexus_add_test(test_runtime unit unit/test_runtime.cpp)
nexus_add_test(test_analysis unit unit/test_analysis.cpp)
nexus_add_test(test_storage unit unit/test_storage.cpp)
nexus_add_test(test_sessions unit unit/test_sessions.cpp)
nexus_add_test(test_roblox unit unit/test_roblox.cpp)

add_executable(nexus-mock-mcp-server support/mock_mcp_server.cpp)
target_link_libraries(nexus-mock-mcp-server PRIVATE nexus_core)
nexus_configure_target(nexus-mock-mcp-server)

nexus_add_test(test_integration integration integration/test_integration.cpp)
add_dependencies(test_integration nexus-runner nexus-cli nexus-mock-mcp-server)

nexus_add_test(test_performance performance performance/test_performance.cpp)
add_dependencies(test_performance nexus-runner)
