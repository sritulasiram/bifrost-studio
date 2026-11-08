add_executable(bifrost-cli
    src/main.cpp
)

if(BIFROST_ENABLE_USB)
    target_link_libraries(bifrost-cli PRIVATE BifrostUsb)
    target_compile_definitions(bifrost-cli PRIVATE BIFROST_HAS_USB=1)
else()
    target_link_libraries(bifrost-cli PRIVATE BifrostCore)
    target_compile_definitions(bifrost-cli PRIVATE BIFROST_HAS_USB=0)
endif()

if(APPLE)
    target_sources(bifrost-cli PRIVATE src/agent_commands.cpp)
    target_link_libraries(bifrost-cli PRIVATE BifrostService)
    target_compile_definitions(bifrost-cli PRIVATE BIFROST_HAS_AGENT=1)
else()
    target_compile_definitions(bifrost-cli PRIVATE BIFROST_HAS_AGENT=0)
endif()
