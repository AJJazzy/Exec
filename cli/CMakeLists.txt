add_executable(nexus-cli main.cpp)
target_link_libraries(nexus-cli PRIVATE nexus_host)
target_compile_definitions(nexus-cli PRIVATE NEXUS_VERSION="${NEXUS_VERSION}")
nexus_configure_target(nexus-cli)
# nexus-cli locates nexus-runner next to itself.
add_dependencies(nexus-cli nexus-runner)
