# nexus_platform: thin OS abstraction (C APIs wrapped in C++). Linux builds exist so the portable
# core can be developed and tested anywhere; Apple-only facilities report themselves unsupported.
add_library(nexus_platform STATIC
    posix/subprocess_posix.cpp
    posix/system_posix.cpp
)
if(APPLE)
    target_sources(nexus_platform PRIVATE macos/system_macos.cpp)
    target_link_libraries(nexus_platform PUBLIC
        "-framework CoreFoundation"
        "-framework CoreServices"
        "-framework Security"
    )
else()
    target_sources(nexus_platform PRIVATE posix/system_linux.cpp)
endif()
add_library(Nexus::platform ALIAS nexus_platform)
target_include_directories(nexus_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
nexus_configure_target(nexus_platform)
