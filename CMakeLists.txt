cmake_minimum_required(VERSION 3.14)

project(data_tamer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(${CMAKE_PROJECT_NAME} STREQUAL ${PROJECT_NAME})
    option(DATA_TAMER_BUILD_TESTS "Build tests" ON)
    option(DATA_TAMER_BUILD_EXAMPLES "Build examples" ON)
else()
    option(DATA_TAMER_BUILD_TESTS "Build tests" OFF)
    option(DATA_TAMER_BUILD_EXAMPLES "Build examples" OFF)
endif()

if (MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -pedantic -Werror)
endif()

add_library(data_tamer STATIC
    include/data_tamer/types.hpp
    include/data_tamer/values.hpp
    include/data_tamer/data_tamer.hpp
    include/data_tamer/channel.hpp

    include/data_tamer/sinks/dummy_sink.hpp

    src/data_tamer.cpp
    src/types.cpp
    src/values.cpp
    src/channel.cpp
    src/sinks/mcap_sink.cpp
    src/data_sync.cpp
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

target_compile_features(data_tamer PUBLIC cxx_std_17)
target_include_directories(data_tamer
 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/3rdparty>
  $<INSTALL_INTERFACE:include>
)

if(DATA_TAMER_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(DATA_TAMER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
