
add_executable(example example.cpp)
target_include_directories(example
 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)
target_link_libraries(example data_tamer)

add_executable(custom_types custom_types.cpp)
target_include_directories(custom_types
 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)
target_link_libraries(custom_types data_tamer)

add_executable(mcap_1m_per_sec mcap_1m_per_sec.cpp)
target_include_directories(mcap_1m_per_sec
 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)
target_link_libraries(mcap_1m_per_sec data_tamer)

if ( ament_cmake_FOUND )
    add_executable(ros2_publisher ros2_publisher.cpp)
    target_include_directories(ros2_publisher
     PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)
    target_link_libraries(ros2_publisher data_tamer)
endif()
