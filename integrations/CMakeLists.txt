# nexus_integrations: Roblox client discovery (read-only) and the Roblox Studio MCP integration.
add_library(nexus_integrations STATIC
    roblox/src/plist.cpp
    roblox/src/macho.cpp
    roblox/src/client.cpp
    roblox/src/connection.cpp
    studio/src/mcp_client.cpp
    studio/src/studio_integration.cpp
)
add_library(Nexus::integrations ALIAS nexus_integrations)
target_include_directories(nexus_integrations PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/roblox/include
    ${CMAKE_CURRENT_SOURCE_DIR}/studio/include
)
target_link_libraries(nexus_integrations PUBLIC nexus_core nexus_platform)
target_compile_definitions(nexus_integrations PRIVATE NEXUS_VERSION="${NEXUS_VERSION}")
nexus_configure_target(nexus_integrations)
