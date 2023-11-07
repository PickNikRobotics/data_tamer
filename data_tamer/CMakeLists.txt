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
    include/data_tamer/channel.hpp
    include/data_tamer/data_tamer.hpp
    include/data_tamer/types.hpp
    include/data_tamer/values.hpp

    src/channel.cpp
    src/data_tamer.cpp
    src/data_sync.cpp
    src/types.cpp
    src/values.cpp

    include/data_tamer/sinks/dummy_sink.hpp
    src/sinks/mcap_sink.cpp
)

target_compile_features(data_tamer PUBLIC cxx_std_17)
target_include_directories(data_tamer
 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/3rdparty>
  $<INSTALL_INTERFACE:include>
)

# detect if compiling in ROS2
find_package(ament_cmake QUIET)
if ( ament_cmake_FOUND )
    target_compile_definitions(data_tamer PUBLIC USING_ROS2=1 )
    set(COMPILING_FOR_ROS2 true)

    find_package(mcap_vendor REQUIRED)
    find_package(rclcpp REQUIRED)

    ament_target_dependencies(data_tamer mcap_vendor rclcpp)

    ament_export_targets(data_tamerTargets HAS_LIBRARY_TARGET)
    ament_export_dependencies(mcap_vendor rclcpp)
    ament_package()
else()
    find_package(mcap REQUIRED)
    target_link_libraries(data_tamer PRIVATE mcap::mcap)
endif()

install(
  DIRECTORY include/
  DESTINATION include
)

install(
  TARGETS data_tamer
  EXPORT data_tamerTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

if(DATA_TAMER_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(DATA_TAMER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()



