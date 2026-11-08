# Bifrost Studio core: transport -> protocol -> firmware/PIT -> planner -> flash.
add_library(BifrostCore STATIC
    src/common/error.cpp
    src/firmware/file_source.cpp
    src/firmware/lz4.cpp
    src/firmware/naming.cpp
    src/firmware/package.cpp
    src/flash/flash.cpp
    src/pit/pit.cpp
    src/planner/plan.cpp
    src/protocol/packet.cpp
    src/protocol/session.cpp
    src/protocol/file_transfer.cpp
    src/transport/interface_selection.cpp
    src/version.cpp
)
target_include_directories(BifrostCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(BifrostCore PUBLIC cxx_std_17)
target_compile_definitions(BifrostCore PRIVATE
    BIFROST_VERSION_STRING="${PROJECT_VERSION}")
target_compile_options(BifrostCore PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(BifrostCore PRIVATE lz4_static)
if(NOT APPLE)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_link_libraries(BifrostCore PRIVATE OpenSSL::Crypto)
endif()

option(BIFROST_ENABLE_USB "Build the libusb transport" ON)
if(BIFROST_ENABLE_USB)
    find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0
        HINTS "${LIBUSB_ROOT}/include" "$ENV{HOME}/.local/include")
    find_library(LIBUSB_LIBRARY NAMES usb-1.0
        HINTS "${LIBUSB_ROOT}/lib" "$ENV{HOME}/.local/lib")
    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
        message(FATAL_ERROR "libusb 1.0.28+ was not found; set LIBUSB_ROOT.")
    endif()

    add_library(BifrostUsb STATIC src/transport/libusb_transport.cpp)
    target_include_directories(BifrostUsb SYSTEM PRIVATE "${LIBUSB_INCLUDE_DIR}")
    target_link_libraries(BifrostUsb PUBLIC BifrostCore PRIVATE "${LIBUSB_LIBRARY}")
    target_compile_options(BifrostUsb PRIVATE -Wall -Wextra -Wpedantic)
    if(APPLE)
        find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
        find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
        # Required by libusb's darwin backend (SecTask entitlement check for
        # device capture) when libusb is linked statically.
        find_library(SECURITY_FRAMEWORK Security REQUIRED)
        target_sources(BifrostUsb PRIVATE src/transport/iokit_registry.cpp)
        target_link_libraries(BifrostUsb PRIVATE
            "${IOKIT_FRAMEWORK}" "${COREFOUNDATION_FRAMEWORK}" "${SECURITY_FRAMEWORK}")
    endif()
endif()

# ---- C API ------------------------------------------------------------------
# BifrostCApi holds the C entry points for static use (tests); the default
# transport comes from whichever default_transport_*.cpp the final binary
# links. The dylib compiles the same sources directly so none are dropped.
add_library(BifrostCApi STATIC src/capi/bifrost_c.cpp)
target_include_directories(BifrostCApi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(BifrostCApi PUBLIC BifrostCore)
target_compile_options(BifrostCApi PRIVATE -Wall -Wextra -Wpedantic)
if(APPLE)
    target_sources(BifrostCApi PRIVATE src/capi/agent_c.cpp)
    target_link_libraries(BifrostCApi PUBLIC BifrostService)
endif()

# libbifrost.dylib: the stable C ABI for the app and XPC service. Only bf_*
# symbols are exported.
if(BIFROST_ENABLE_USB)
    add_library(Bifrost SHARED src/capi/bifrost_c.cpp src/capi/default_transport_libusb.cpp)
    target_link_libraries(Bifrost PRIVATE BifrostUsb)
else()
    add_library(Bifrost SHARED src/capi/bifrost_c.cpp src/capi/default_transport_none.cpp)
    target_link_libraries(Bifrost PRIVATE BifrostCore)
endif()
target_include_directories(Bifrost PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(Bifrost PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(Bifrost PROPERTIES
    OUTPUT_NAME bifrost
    VERSION "${PROJECT_VERSION}"
    SOVERSION 1
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
if(APPLE)
    # The agent client part of the C API (service/ is added after core/).
    target_sources(Bifrost PRIVATE src/capi/agent_c.cpp)
    target_link_libraries(Bifrost PRIVATE BifrostService)
    set_target_properties(Bifrost PROPERTIES INSTALL_NAME_DIR "@rpath")
    target_link_options(Bifrost PRIVATE
        "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/capi/exports.txt")
    set_property(TARGET Bifrost APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/capi/exports.txt")
endif()
