# Nexus.app — native macOS application.
#   SwiftUI app shell + AppKit (TextKit 1) code editor   ui/app/Sources/*.swift
#   Objective-C++ bridge to the C++ core                 ui/bridge/NXBridge.{h,mm}
# Works with the Xcode generator (cmake -G Xcode, see scripts/build/xcode.sh) and with Ninja.
enable_language(Swift)
enable_language(OBJCXX)
set(CMAKE_Swift_LANGUAGE_VERSION 5)

set(NEXUS_APP_SWIFT_SOURCES
    app/Sources/NexusApp.swift
    app/Sources/AppModel.swift
    app/Sources/Documents.swift
    app/Sources/Theme.swift
    app/Sources/RootView.swift
    app/Sources/EditorView.swift
    app/Sources/CodeEditor.swift
    app/Sources/ConsoleView.swift
    app/Sources/LibraryView.swift
    app/Sources/RobloxView.swift
    app/Sources/CompatibilityView.swift
    app/Sources/DiagnosticsView.swift
    app/Sources/SettingsView.swift
    app/Sources/SelfTest.swift
)
set(NEXUS_BRIDGING_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/app/Nexus-Bridging-Header.h)

configure_file(app/Resources/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/nexus_app_version.h
     "#pragma once\n#define NEXUS_VERSION \"${NEXUS_VERSION}\"\n")

add_executable(Nexus MACOSX_BUNDLE
    ${NEXUS_APP_SWIFT_SOURCES}
    bridge/NXBridge.mm
    bridge/NXBridge.h
    ${NEXUS_BRIDGING_HEADER}
)
set_source_files_properties(bridge/NXBridge.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-include;${CMAKE_CURRENT_BINARY_DIR}/generated/nexus_app_version.h")

set_target_properties(Nexus PROPERTIES
    OUTPUT_NAME Nexus
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER io.github.ajjazzy.nexus
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER ${NEXUS_BRIDGING_HEADER}
    XCODE_ATTRIBUTE_SWIFT_VERSION 5.0
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES
    # Signing is done explicitly by scripts/package/package-macos.sh (ad hoc or Developer ID), so
    # the build itself stays reproducible on machines without signing identities.
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
    XCODE_ATTRIBUTE_COMBINE_HIDPI_IMAGES YES
)
if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
    target_compile_options(Nexus PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-import-objc-header ${NEXUS_BRIDGING_HEADER}>")
endif()

target_include_directories(Nexus PRIVATE bridge)
target_link_libraries(Nexus PRIVATE
    nexus_host
    c++
    "-framework AppKit"
    "-framework SwiftUI"
    "-framework UniformTypeIdentifiers"
)

# The isolated execution helper ships inside the bundle (Contents/MacOS/nexus-runner).
add_dependencies(Nexus nexus-runner)
add_custom_command(TARGET Nexus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:nexus-runner> $<TARGET_BUNDLE_CONTENT_DIR:Nexus>/MacOS/nexus-runner
    COMMENT "Embedding nexus-runner in Nexus.app"
    VERBATIM
)
