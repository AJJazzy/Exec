# Nexus — native Luau scripting platform for macOS.
#
# Layout:
#   platform/      OS abstraction (process spawning, process discovery, code signing, sandboxing)
#   core/          portable C++20 core (compiler, runtime, execution, sessions, compatibility, library)
#   integrations/  Roblox client detection and Roblox Studio (official MCP server) integration
#   host/          composition root shared by the CLI and the macOS application
#   runner/        nexus-runner: isolated, resource-limited Luau execution process
#   cli/           nexus-cli: command line front end
#   ui/            macOS application (SwiftUI + AppKit, Objective-C++ bridge)
#   tests/         unit, integration and performance tests (CTest)
cmake_minimum_required(VERSION 3.28)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NEXUS_VERSION LIMIT_COUNT 1)

if(APPLE)
    # Must be set before project(). macOS 13 is required by the SwiftUI APIs the app uses.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

project(Nexus VERSION ${NEXUS_VERSION} LANGUAGES C CXX)

option(NEXUS_BUILD_TESTS "Build the Nexus test suite" ON)
option(NEXUS_BUILD_CLI "Build the nexus-cli command line tool" ON)
option(NEXUS_WERROR "Treat compiler warnings as errors in Nexus sources" OFF)
if(APPLE)
    option(NEXUS_BUILD_APP "Build the native macOS application" ON)
else()
    set(NEXUS_BUILD_APP OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Put all executables in one directory so the host can locate nexus-runner next to itself.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

include(cmake/NexusWarnings.cmake)
include(cmake/NexusLuau.cmake)

add_subdirectory(platform)
add_subdirectory(core)
add_subdirectory(integrations)
add_subdirectory(host)
add_subdirectory(runner)

if(NEXUS_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(NEXUS_BUILD_APP)
    add_subdirectory(ui)
endif()

if(NEXUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "Nexus ${NEXUS_VERSION}: tests=${NEXUS_BUILD_TESTS} cli=${NEXUS_BUILD_CLI} app=${NEXUS_BUILD_APP}")
