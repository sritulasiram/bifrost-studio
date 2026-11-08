# Background agent (LaunchAgent) and privileged USB helper (LaunchDaemon).
set(BIFROST_TEAM_ID "" CACHE STRING
    "Developer ID team identifier; empty = development build (identifier-only peer checks)")
set(BIFROST_AGENT_LABEL "local.bifrost.agent" CACHE STRING "Agent launchd label / Mach service")
set(BIFROST_USBD_LABEL "local.bifrost.usbd" CACHE STRING "USB helper launchd label / Mach service")
set(BIFROST_CLI_IDENTIFIER "local.bifrost.cli" CACHE STRING "CLI code-signing identifier")
if(NOT BIFROST_TEAM_ID)
    message(STATUS "Service peer checks: development (signing identifier only)")
endif()

configure_file(src/service_config.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/service_config.hpp" @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/packaging/launchd/agent.plist.in" "${CMAKE_BINARY_DIR}/${BIFROST_AGENT_LABEL}.plist" @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/packaging/launchd/usbd.plist.in" "${CMAKE_BINARY_DIR}/${BIFROST_USBD_LABEL}.plist" @ONLY)

add_library(BifrostService STATIC
    src/wire.cpp
    src/listener.cpp
    src/usb_service.cpp
    src/remote_transport.cpp
    src/agent.cpp
    src/agent_client.cpp
)
target_include_directories(BifrostService PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(BifrostService PUBLIC BifrostCore)
target_compile_options(BifrostService PRIVATE -Wall -Wextra -Wpedantic)

if(BIFROST_ENABLE_USB)
    add_executable(bifrost-usbd src/usbd_main.cpp)
    target_link_libraries(bifrost-usbd PRIVATE BifrostService BifrostUsb)
    target_compile_options(bifrost-usbd PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(bifrost-agent src/agent_main.cpp)
    target_link_libraries(bifrost-agent PRIVATE BifrostService BifrostUsb)
    target_compile_options(bifrost-agent PRIVATE -Wall -Wextra -Wpedantic)
endif()
