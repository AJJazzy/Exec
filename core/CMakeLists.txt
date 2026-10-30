# nexus_core: portable C++20 core. Depends only on Luau and nexus_platform (process spawning for the
# isolated execution target).
add_library(nexus_core STATIC
    src/common/cancellation.cpp
    src/common/json.cpp
    src/common/strings.cpp
    src/diagnostics/diagnostic.cpp
    src/diagnostics/logger.cpp
    src/compiler/compiler.cpp
    src/compiler/source_map.cpp
    src/compatibility/catalog.cpp
    src/compatibility/analyzer.cpp
    src/editor/highlighter.cpp
    src/editor/formatter.cpp
    src/editor/completion.cpp
    src/runtime/types.cpp
    src/runtime/luau_runtime.cpp
    src/execution/target.cpp
    src/execution/local_targets.cpp
    src/execution/runner_protocol.cpp
    src/sessions/session_manager.cpp
    src/library/script_library.cpp
    src/library/draft_store.cpp
    src/settings/settings.cpp
)
add_library(Nexus::core ALIAS nexus_core)

target_include_directories(nexus_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(nexus_core
    PUBLIC nexus_platform
    PRIVATE Luau.Compiler Luau.Ast Luau.VM
)
target_compile_definitions(nexus_core PRIVATE NEXUS_LUAU_TAG="${NEXUS_LUAU_TAG}" NEXUS_VERSION="${NEXUS_VERSION}")
nexus_configure_target(nexus_core)

find_package(Threads REQUIRED)
target_link_libraries(nexus_core PUBLIC Threads::Threads)
