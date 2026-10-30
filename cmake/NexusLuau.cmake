# Luau dependency (https://github.com/luau-lang/luau, MIT licence).
#
# The version is pinned by tag *and* commit so builds are reproducible. To build offline, clone the
# same commit and configure with -DFETCHCONTENT_SOURCE_DIR_LUAU=/path/to/luau.
#
# Only the Common, Ast, Compiler and VM libraries are used:
#   Ast       parser/lexer for diagnostics, highlighting, formatting and compatibility analysis
#   Compiler  source -> bytecode
#   VM        sandboxed bytecode execution
# Analysis (type checker) and CodeGen (native JIT) are intentionally not linked to keep the build
# and the attack surface small.
include(FetchContent)

set(NEXUS_LUAU_TAG "0.744" CACHE STRING "Pinned Luau release tag")
set(NEXUS_LUAU_COMMIT "32c28089305f488f901aaf1dbf3fd1cb65fa3d11" CACHE STRING "Pinned Luau commit for NEXUS_LUAU_TAG")

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_SHARED OFF CACHE BOOL "" FORCE)

FetchContent_Declare(luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG ${NEXUS_LUAU_COMMIT}
    GIT_SHALLOW FALSE
    # Only the Luau targets Nexus links against are built; SYSTEM keeps Nexus' stricter warning flags
    # from applying to Luau headers.
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_MakeAvailable(luau)

foreach(target Luau.Common Luau.Ast Luau.Compiler Luau.VM)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "Luau target ${target} not found; the pinned Luau version may have changed its build layout")
    endif()
endforeach()

message(STATUS "Luau ${NEXUS_LUAU_TAG} (${NEXUS_LUAU_COMMIT}) from ${luau_SOURCE_DIR}")
