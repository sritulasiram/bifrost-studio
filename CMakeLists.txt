cmake_minimum_required(VERSION 3.26)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal macOS architectures")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

project(BifrostStudio VERSION 1.0.0 LANGUAGES C CXX)

option(BIFROST_BUILD_APP "Build the SwiftUI app bundle" ${APPLE})
set(BIFROST_BUNDLE_ID "local.bifrost.test" CACHE STRING
    "Bundle identifier (replace with an identifier in a domain you control for release)")

option(BIFROST_FUZZ "Build fuzz targets with ASan+UBSan (libFuzzer when available)" OFF)
if(BIFROST_FUZZ)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
    check_cxx_source_compiles(
        "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long) { return 0; }"
        BIFROST_HAVE_LIBFUZZER)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    # Instrument everything, including LZ4, before it is fetched.
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    if(BIFROST_HAVE_LIBFUZZER)
        add_compile_options(-fsanitize=fuzzer-no-link)
        message(STATUS "Fuzzing: libFuzzer")
    else()
        message(STATUS "Fuzzing: libFuzzer unavailable; using the standalone driver")
    endif()
endif()

include(FetchContent)
set(LZ4_BUILD_CLI OFF CACHE BOOL "Do not build the LZ4 command-line tool" FORCE)
FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    # v1.10.0, pinned by commit so the fetched source cannot change.
    GIT_TAG ebb370ca83af193212df4dcbadcc5d87bc0de2f0
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(lz4)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

# Source layout:
#   src/core     C++17 library (transport, protocol, firmware, PIT, planner, flash, C API)
#   src/service  background agent (LaunchAgent) and root USB helper (LaunchDaemon)
#   src/cli      bifrost-cli
#   src/app      SwiftUI app and the .app bundle
#   packaging/   launchd plists, DMG script
add_subdirectory(src/core)
if(APPLE)
    add_subdirectory(src/service)
endif()
add_subdirectory(src/cli)
if(APPLE AND BIFROST_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
