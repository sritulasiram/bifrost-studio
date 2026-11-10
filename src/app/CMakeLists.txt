# Bifrost Studio.app: the SwiftUI app plus, inside the bundle, bifrost-cli,
# libbifrost, bifrost-agent, bifrost-usbd and their launchd plists.
# The bundle is written to the top build directory: "$BUILD/Bifrost Studio.app".

if(NOT BIFROST_ENABLE_USB)
    message(FATAL_ERROR "The app bundle needs BIFROST_ENABLE_USB=ON (it ships the USB helpers)")
endif()

set(BIFROST_CORE_INCLUDE "${PROJECT_SOURCE_DIR}/src/core/include")
set(BIFROST_NOTICES "${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt")
set(BIFROST_ICON_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/Resources/create_app_icon.swift")

# Android platform-tools (adb) ship inside the app so "Reboot to Download Mode"
# works without a separate install. Downloaded once at configure time, pinned.
set(BIFROST_PT_VERSION "37.0.1")
set(BIFROST_PT_SHA256 "ee39ad5967e95c2a07f04dbcbde96b1a0c916ba376096db5d2f498b7727a5d1d")
set(BIFROST_PT_ZIP "${CMAKE_BINARY_DIR}/platform-tools_r${BIFROST_PT_VERSION}-darwin.zip")
set(BIFROST_PT_DIR "${CMAKE_BINARY_DIR}/platform-tools")
if(NOT EXISTS "${BIFROST_PT_DIR}/adb")
    file(DOWNLOAD
        "https://dl.google.com/android/repository/platform-tools_r${BIFROST_PT_VERSION}-darwin.zip"
        "${BIFROST_PT_ZIP}" EXPECTED_HASH SHA256=${BIFROST_PT_SHA256} SHOW_PROGRESS)
    file(ARCHIVE_EXTRACT INPUT "${BIFROST_PT_ZIP}" DESTINATION "${CMAKE_BINARY_DIR}")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    "${CMAKE_BINARY_DIR}/BifrostStudio-Info.plist"
    @ONLY
)

# create_app_icon.swift is a script with top-level code; it runs as a build
# step and must not be compiled into the app.
file(GLOB BIFROST_APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Sources/*.swift")

if(CMAKE_GENERATOR STREQUAL "Xcode")
    enable_language(Swift)
    find_library(SWIFTUI_FRAMEWORK SwiftUI REQUIRED)
    find_program(SWIFT_EXECUTABLE swift REQUIRED)

    add_executable(BifrostStudio MACOSX_BUNDLE ${BIFROST_APP_SOURCES})
    target_link_libraries(BifrostStudio PRIVATE "${SWIFTUI_FRAMEWORK}" Bifrost)
    target_compile_options(BifrostStudio PRIVATE "SHELL:-I ${BIFROST_CORE_INCLUDE}")
    add_dependencies(BifrostStudio bifrost-cli bifrost-agent bifrost-usbd)
    add_custom_command(TARGET BifrostStudio POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:bifrost-cli>"
            "$<TARGET_BUNDLE_DIR:BifrostStudio>/Contents/MacOS/bifrost-cli"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:bifrost-agent>"
            "$<TARGET_BUNDLE_DIR:BifrostStudio>/Contents/MacOS/bifrost-agent"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:bifrost-usbd>"
            "$<TARGET_BUNDLE_DIR:BifrostStudio>/Contents/MacOS/bifrost-usbd"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_DIR:BifrostStudio>/Contents/Library/LaunchAgents"
            "$<TARGET_BUNDLE_DIR:BifrostStudio>/Contents/Library/LaunchDaemons"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_BINARY_DIR}/${BIFROST_AGENT_LABEL}.plist"
            "$<TARGET_BUNDLE_DIR:BifrostStudio>/Contents/Library/LaunchAgents/${BIFROST_AGENT_LABEL}.plist"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_BINARY_DIR}/${BIFROST_USBD_LABEL}.plist"
            "$<TARGET_BUNDLE_DIR:BifrostStudio>/Contents/Library/LaunchDaemons/${BIFROST_USBD_LABEL}.plist"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_DIR:BifrostStudio>/Contents/Resources"
        COMMAND "${SWIFT_EXECUTABLE}" "${BIFROST_ICON_SCRIPT}"
            "$<TARGET_BUNDLE_DIR:BifrostStudio>/Contents/Resources/AppIcon.icns"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BIFROST_NOTICES}"
            "$<TARGET_BUNDLE_DIR:BifrostStudio>/Contents/Resources/THIRD-PARTY-NOTICES.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${BIFROST_PT_DIR}"
            "$<TARGET_BUNDLE_DIR:BifrostStudio>/Contents/Resources/platform-tools"
        VERBATIM
    )
    set_target_properties(BifrostStudio PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "${BIFROST_BUNDLE_ID}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
    )
    return()
endif()

find_program(SWIFTC_EXECUTABLE swiftc REQUIRED)
find_program(SWIFT_EXECUTABLE swift REQUIRED)
find_program(LIPO_EXECUTABLE lipo REQUIRED)
find_program(CODESIGN_EXECUTABLE codesign REQUIRED)

set(BIFROST_APP_BUNDLE "${CMAKE_BINARY_DIR}/Bifrost Studio.app")
set(BIFROST_APP_EXECUTABLE "${BIFROST_APP_BUNDLE}/Contents/MacOS/BifrostStudio")
set(BIFROST_APP_CLI "${BIFROST_APP_BUNDLE}/Contents/MacOS/bifrost-cli")
set(BIFROST_APP_PLIST "${BIFROST_APP_BUNDLE}/Contents/Info.plist")
set(BIFROST_APP_NOTICES "${BIFROST_APP_BUNDLE}/Contents/Resources/THIRD-PARTY-NOTICES.txt")
set(BIFROST_APP_ICON "${BIFROST_APP_BUNDLE}/Contents/Resources/AppIcon.icns")
set(BIFROST_APP_AGENT "${BIFROST_APP_BUNDLE}/Contents/MacOS/bifrost-agent")
set(BIFROST_APP_USBD "${BIFROST_APP_BUNDLE}/Contents/MacOS/bifrost-usbd")
set(BIFROST_APP_AGENT_PLIST
    "${BIFROST_APP_BUNDLE}/Contents/Library/LaunchAgents/${BIFROST_AGENT_LABEL}.plist")
set(BIFROST_APP_USBD_PLIST
    "${BIFROST_APP_BUNDLE}/Contents/Library/LaunchDaemons/${BIFROST_USBD_LABEL}.plist")
set(BIFROST_APP_LIB "${BIFROST_APP_BUNDLE}/Contents/Frameworks/libbifrost.1.dylib") # SOVERSION 1
set(BIFROST_SWIFT_FLAGS -parse-as-library
    -I "${BIFROST_CORE_INCLUDE}"
    -L "$<TARGET_FILE_DIR:Bifrost>"
    -Xlinker -rpath -Xlinker "@executable_path/../Frameworks")
set(BIFROST_APP_ARM64 "${CMAKE_BINARY_DIR}/BifrostStudio-arm64")
set(BIFROST_APP_X86_64 "${CMAKE_BINARY_DIR}/BifrostStudio-x86_64")

add_custom_command(
    OUTPUT "${BIFROST_APP_EXECUTABLE}" "${BIFROST_APP_PLIST}"
        "${BIFROST_APP_ICON}"
        "${BIFROST_APP_CLI}" "${BIFROST_APP_NOTICES}"
        "${BIFROST_APP_AGENT}" "${BIFROST_APP_USBD}" "${BIFROST_APP_LIB}"
        "${BIFROST_APP_AGENT_PLIST}" "${BIFROST_APP_USBD_PLIST}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${BIFROST_APP_BUNDLE}/Contents/MacOS"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${BIFROST_APP_BUNDLE}/Contents/Resources"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${BIFROST_APP_BUNDLE}/Contents/Frameworks"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${BIFROST_APP_BUNDLE}/Contents/Library/LaunchAgents"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${BIFROST_APP_BUNDLE}/Contents/Library/LaunchDaemons"
    COMMAND "${SWIFT_EXECUTABLE}" "${BIFROST_ICON_SCRIPT}" "${BIFROST_APP_ICON}"
    COMMAND "${SWIFTC_EXECUTABLE}" ${BIFROST_SWIFT_FLAGS}
        -target arm64-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}
        -o "${BIFROST_APP_ARM64}" ${BIFROST_APP_SOURCES}
    COMMAND "${SWIFTC_EXECUTABLE}" ${BIFROST_SWIFT_FLAGS}
        -target x86_64-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}
        -o "${BIFROST_APP_X86_64}" ${BIFROST_APP_SOURCES}
    COMMAND "${LIPO_EXECUTABLE}" -create
        "${BIFROST_APP_ARM64}" "${BIFROST_APP_X86_64}"
        -output "${BIFROST_APP_EXECUTABLE}"
    COMMAND "${CMAKE_COMMAND}" -E rm "${BIFROST_APP_ARM64}" "${BIFROST_APP_X86_64}"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${CMAKE_BINARY_DIR}/BifrostStudio-Info.plist" "${BIFROST_APP_PLIST}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:bifrost-cli>" "${BIFROST_APP_CLI}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:Bifrost>" "${BIFROST_APP_LIB}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:bifrost-agent>" "${BIFROST_APP_AGENT}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:bifrost-usbd>" "${BIFROST_APP_USBD}"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${CMAKE_BINARY_DIR}/${BIFROST_AGENT_LABEL}.plist" "${BIFROST_APP_AGENT_PLIST}"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${CMAKE_BINARY_DIR}/${BIFROST_USBD_LABEL}.plist" "${BIFROST_APP_USBD_PLIST}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BIFROST_NOTICES}" "${BIFROST_APP_NOTICES}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${BIFROST_APP_BUNDLE}/Contents/Resources/platform-tools"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${BIFROST_PT_DIR}"
        "${BIFROST_APP_BUNDLE}/Contents/Resources/platform-tools"
    # Ad-hoc signatures with explicit identifiers: the agent and the USB
    # helper check their peers against these identifiers.
    COMMAND "${CODESIGN_EXECUTABLE}" --force --sign - "${BIFROST_APP_LIB}"
    COMMAND "${CODESIGN_EXECUTABLE}" --force --sign - -i "${BIFROST_CLI_IDENTIFIER}"
        "${BIFROST_APP_CLI}"
    COMMAND "${CODESIGN_EXECUTABLE}" --force --sign - -i "${BIFROST_AGENT_LABEL}"
        "${BIFROST_APP_AGENT}"
    COMMAND "${CODESIGN_EXECUTABLE}" --force --sign - -i "${BIFROST_USBD_LABEL}"
        "${BIFROST_APP_USBD}"
    COMMAND "${CODESIGN_EXECUTABLE}" --force --sign - "${BIFROST_APP_BUNDLE}"
    DEPENDS
        ${BIFROST_APP_SOURCES}
        Bifrost
        "${BIFROST_ICON_SCRIPT}"
        bifrost-cli
        bifrost-agent
        bifrost-usbd
        "${CMAKE_BINARY_DIR}/${BIFROST_AGENT_LABEL}.plist"
        "${CMAKE_BINARY_DIR}/${BIFROST_USBD_LABEL}.plist"
        "${BIFROST_NOTICES}"
        "${CMAKE_BINARY_DIR}/BifrostStudio-Info.plist"
    VERBATIM
)

# Not notarized: no Developer ID (owner has no Apple Developer account).
add_custom_target(dmg
    COMMAND "${PROJECT_SOURCE_DIR}/packaging/make-dmg.sh"
        "${BIFROST_APP_BUNDLE}"
        "${CMAKE_BINARY_DIR}/BifrostStudio-${PROJECT_VERSION}.dmg" "${PROJECT_VERSION}"
    DEPENDS BifrostStudio
    COMMENT "Building BifrostStudio-${PROJECT_VERSION}.dmg"
    VERBATIM)
add_custom_target(BifrostStudio ALL
    DEPENDS "${BIFROST_APP_EXECUTABLE}" "${BIFROST_APP_PLIST}"
        "${BIFROST_APP_CLI}" "${BIFROST_APP_ICON}"
        "${BIFROST_APP_AGENT}" "${BIFROST_APP_USBD}"
)
