
function(CompileExample TARGET)
    add_executable(${TARGET} ${TARGET}.cpp)
    target_include_directories(${TARGET}
     PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
    target_link_libraries(${TARGET} data_tamer_cpp)
endfunction()

CompileExample(example)
CompileExample(custom_types)
CompileExample(mcap_1m_per_sec)
CompileExample(mcap_writer_sample)

add_executable(mcap_reader mcap_reader.cpp)
target_include_directories(mcap_reader
 PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

if ( ament_cmake_FOUND )
    ament_target_dependencies(mcap_reader mcap_vendor)

    CompileExample(ros2_publisher)

    install(TARGETS ros2_publisher
            DESTINATION lib/${PROJECT_NAME})
else()
    target_link_libraries(mcap_reader data_tamer_cpp mcap::mcap)
endif()

