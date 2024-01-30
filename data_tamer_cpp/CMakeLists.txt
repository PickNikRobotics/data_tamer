cmake_minimum_required(VERSION 3.14)

project(data_tamer_cpp VERSION 0.9.2)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)

if(${CMAKE_PROJECT_NAME} STREQUAL ${PROJECT_NAME})
    option(DATA_TAMER_BUILD_TESTS "Build tests" ON)
    option(DATA_TAMER_BUILD_EXAMPLES "Build examples" ON)
else()
    option(DATA_TAMER_BUILD_TESTS "Build tests" OFF)
    option(DATA_TAMER_BUILD_EXAMPLES "Build examples" OFF)
endif()

option(BUILD_SHARED_LIBS "Build using shared libraries" ON)

if (MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wcast-align -Wconversion -Wdouble-promotion -Wextra -Wimplicit-fallthrough
        -Wno-deprecated-declarations -Wno-ignored-attributes -Wno-unused-parameter
        -Wnon-virtual-dtor -Wno-unused-variable -Wnull-dereference -Wold-style-cast
        -Woverloaded-virtual -Wshadow -Wsign-conversion
        -Werror -Wpedantic)
endif()


find_package(ament_cmake QUIET)

if ( ament_cmake_FOUND )
    set(ROS2_SINK 
        src/sinks/ros2_publisher_sink.cpp)
endif()

if(BUILD_SHARED_LIBS OR ament_cmake_FOUND)
    set(LIB_TYPE SHARED)
else()
    set(LIB_TYPE STATIC)
endif()

if(ament_cmake_FOUND AND NOT BUILD_SHARED_LIBS)
    message("BUILD_SHARED_LIBS forced to ON (i.e. SHARED) in ROS2")
endif()

add_library(data_tamer ${LIB_TYPE}
    include/data_tamer/channel.hpp
    include/data_tamer/custom_types.hpp
    include/data_tamer/data_tamer.hpp
    include/data_tamer/types.hpp
    include/data_tamer/values.hpp
    include/data_tamer/sinks/dummy_sink.hpp
    include/data_tamer/sinks/mcap_sink.hpp

    src/channel.cpp
    src/data_tamer.cpp
    src/data_sink.cpp
    src/types.cpp

    src/sinks/mcap_sink.cpp
    ${ROS2_SINK}
    include/data_tamer/details/mutex.hpp
)

target_compile_features(data_tamer PUBLIC cxx_std_17)
target_include_directories(data_tamer
 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
 PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/3rdparty>
)
target_compile_definitions(data_tamer PUBLIC -DDATA_TAMER_VERSION="${CMAKE_PROJECT_VERSION}")

# detect if compiling in ROS2
find_package(ament_cmake QUIET)
if ( ament_cmake_FOUND )
    target_compile_definitions(data_tamer PUBLIC USING_ROS2=1 )
    set(COMPILING_FOR_ROS2 true)

    find_package(mcap_vendor REQUIRED)
    find_package(rclcpp REQUIRED)
    find_package(data_tamer_msgs REQUIRED)

    ament_target_dependencies(data_tamer mcap_vendor rclcpp data_tamer_msgs)

    ament_export_targets(data_tamerTargets HAS_LIBRARY_TARGET)
    ament_export_dependencies(mcap_vendor rclcpp data_tamer_msgs)
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
)

if (NOT ament_cmake_FOUND )

    set(LIBRARY_INSTALL_DIR lib)
    set(INCLUDE_INSTALL_DIR include)

    # Install config file for finding the package.
    include(CMakePackageConfigHelpers)

    configure_package_config_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/data_tamerConfig.cmake.in
      ${CMAKE_CURRENT_BINARY_DIR}/data_tamerConfig.cmake
      INSTALL_DESTINATION lib/cmake/data_tamer
      PATH_VARS INCLUDE_INSTALL_DIR)

    write_basic_package_version_file(
        "data_tamerConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(EXPORT data_tamerTargets
            FILE data_tamerTargets.cmake
            NAMESPACE data_tamer::
            DESTINATION lib/cmake/data_tamer )

    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/data_tamerConfig.cmake"
            DESTINATION lib/cmake/data_tamer )

endif()

if(DATA_TAMER_BUILD_TESTS)
    include(CTest)

target_sources(Continuous
  PRIVATE
    include/data_tamer/custom_types.hpp
)
enable_testing()
    add_subdirectory(tests)
endif()

if(DATA_TAMER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

find_package(benchmark QUIET)
if(benchmark_FOUND)
    add_subdirectory(benchmarks)
else()
    message("Google Benchmark library not found")
endif()

