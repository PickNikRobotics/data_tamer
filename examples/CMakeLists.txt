

add_executable(example example.cpp)

target_include_directories(example
 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)

target_link_libraries(example data_tamer)
