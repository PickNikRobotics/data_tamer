add_executable(dt_benchmark data_tamer_benchmark.cpp ${PROJECT_SOURCE_DIR}/tests/alloc_counter.cpp)
target_include_directories(dt_benchmark PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(dt_benchmark data_tamer benchmark::benchmark)

add_executable(rt_latency rt_latency.cpp ${PROJECT_SOURCE_DIR}/tests/alloc_counter.cpp)
target_include_directories(rt_latency PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(rt_latency data_tamer pthread)
