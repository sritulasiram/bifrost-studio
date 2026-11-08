# Fuzz targets (configure with -DBIFROST_FUZZ=ON, single architecture).
# With libFuzzer (upstream LLVM clang) they are coverage-guided; with Apple
# clang they link standalone_driver.cpp, a weaker random-mutation driver.
set(BIFROST_FUZZ_RUNS 20000 CACHE STRING "Mutated runs per fuzz target under ctest")

foreach(target pit package lz4 naming)
    add_executable(fuzz_${target} fuzz_${target}.cpp)
    target_link_libraries(fuzz_${target} PRIVATE BifrostCore)
    if(BIFROST_HAVE_LIBFUZZER)
        target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
    else()
        target_sources(fuzz_${target} PRIVATE standalone_driver.cpp)
    endif()
    # libFuzzer writes new inputs to the first corpus directory, so give it a
    # scratch directory and keep the committed seeds read-only.
    set(work "${CMAKE_CURRENT_BINARY_DIR}/work/${target}")
    file(MAKE_DIRECTORY "${work}")
    add_test(NAME fuzz_${target}
        COMMAND fuzz_${target} -runs=${BIFROST_FUZZ_RUNS} -max_len=65536 -seed=1
            "${work}" "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${target}")
endforeach()

add_executable(make_fuzz_corpus make_corpus.cpp)
target_include_directories(make_fuzz_corpus PRIVATE "${PROJECT_SOURCE_DIR}/tests"
    "$<TARGET_PROPERTY:lz4_static,INTERFACE_INCLUDE_DIRECTORIES>")
target_link_libraries(make_fuzz_corpus PRIVATE BifrostCore lz4_static)
