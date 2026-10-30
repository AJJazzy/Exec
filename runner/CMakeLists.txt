add_executable(nexus-runner main.cpp)
target_link_libraries(nexus-runner PRIVATE nexus_core nexus_platform)
target_compile_definitions(nexus-runner PRIVATE NEXUS_VERSION="${NEXUS_VERSION}")
nexus_configure_target(nexus-runner)
