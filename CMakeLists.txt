cmake_minimum_required(VERSION 3.14)

project(data_tamer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DATA_TAMER_BUILD_TESTS "Build tests" ON)
option(DATA_TAMER_BUILD_EXAMPLES "Build examples" ON)

add_library(data_tamer STATIC
    include/data_tamer/types.hpp
    include/data_tamer/values.hpp
    include/data_tamer/data_tamer.hpp
    include/data_tamer/channel.hpp
    include/data_tamer/dictionary.hpp
    include/data_tamer/sinks/dummy_sink.hpp

    src/data_tamer.cpp
    src/types.cpp
    src/values.cpp
    src/channel.cpp
    src/sinks/mcap_sink.cpp
)

# detect if compiling in ROS2
find_package(ament_cmake QUIET)
if ( ament_cmake_FOUND )
    target_compile_definitions(data_tamer PUBLIC USING_ROS2=1 )
    set(COMPILING_FOR_ROS2 true)
else()
    find_package(mcap REQUIRED)
    target_link_libraries(data_tamer PRIVATE mcap::mcap)
endif()


target_include_directories(data_tamer
 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

if(DATA_TAMER_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(DATA_TAMER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
