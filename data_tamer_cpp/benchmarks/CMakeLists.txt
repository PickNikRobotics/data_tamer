# Every benchmark links the library and the test allocation hook, so that
# allocations on the measured thread can be counted.
function(CompileBenchmark TARGET)
    add_executable(${TARGET} ${TARGET}.cpp ${PROJECT_SOURCE_DIR}/tests/alloc_counter.cpp)
    target_include_directories(${TARGET}
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(${TARGET} data_tamer ${ARGN})
endfunction()

find_package(Threads REQUIRED)

CompileBenchmark(data_tamer_benchmark benchmark::benchmark)
CompileBenchmark(rt_latency Threads::Threads)
