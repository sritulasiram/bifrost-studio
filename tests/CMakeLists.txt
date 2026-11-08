# Bootloader simulator shared by the simulator-driven suites.
add_library(bifrost_sim STATIC support/sim_device.cpp)
target_include_directories(bifrost_sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(bifrost_sim PUBLIC BifrostCore)
target_compile_options(bifrost_sim PRIVATE -Wall -Wextra)

function(bifrost_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${name} PRIVATE bifrost_sim ${ARGN})
    target_include_directories(${name} PRIVATE
        "$<TARGET_PROPERTY:lz4_static,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    target_compile_definitions(${name} PRIVATE
        BIFROST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

bifrost_test(test_version)
bifrost_test(test_transport)
bifrost_test(test_package)
bifrost_test(test_planner)
bifrost_test(test_session)
bifrost_test(test_flash)
bifrost_test(test_sim_flash)

# C API through the simulator (no libusb: links the null default transport).
add_executable(test_capi test_capi.cpp
    "${PROJECT_SOURCE_DIR}/src/core/src/capi/default_transport_none.cpp")
target_include_directories(test_capi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src/core/src"
    "$<TARGET_PROPERTY:lz4_static,INTERFACE_INCLUDE_DIRECTORIES>")
target_link_libraries(test_capi PRIVATE BifrostCApi bifrost_sim)
target_compile_options(test_capi PRIVATE -Wall -Wextra)
add_test(NAME test_capi COMMAND test_capi)

# The public header compiles as C, and the dylib exports only bf_* symbols.
add_executable(test_capi_header test_capi_header.c)
set_source_files_properties(test_capi_header.c PROPERTIES LANGUAGE C)
target_link_libraries(test_capi_header PRIVATE Bifrost)
target_compile_options(test_capi_header PRIVATE -Wall -Wextra -pedantic -std=c99)
add_test(NAME test_capi_header COMMAND test_capi_header)
if(APPLE)
    add_test(NAME test_capi_exports
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check_exports.sh" "$<TARGET_FILE:Bifrost>")
endif()

if(BIFROST_FUZZ)
    add_subdirectory(fuzz)
endif()

# Background agent and USB helper over in-process XPC.
if(APPLE)
    bifrost_test(test_service BifrostService)
endif()
if(APPLE)
    add_executable(test_capi_agent test_capi_agent.cpp
        "${PROJECT_SOURCE_DIR}/src/core/src/capi/default_transport_none.cpp")
    target_include_directories(test_capi_agent PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/src/core/src"
        "$<TARGET_PROPERTY:lz4_static,INTERFACE_INCLUDE_DIRECTORIES>")
    target_link_libraries(test_capi_agent PRIVATE BifrostCApi bifrost_sim)
    target_compile_options(test_capi_agent PRIVATE -Wall -Wextra)
    add_test(NAME test_capi_agent COMMAND test_capi_agent)
endif()
