
function(CompileExample TARGET)
    add_executable(${TARGET} ${TARGET}.cpp)
    target_include_directories(${TARGET}
     PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)
    target_link_libraries(${TARGET} data_tamer)
endfunction()


CompileExample(example)
CompileExample(custom_types)
CompileExample(mcap_1m_per_sec)
CompileExample(raii)

if ( ament_cmake_FOUND )
    CompileExample(ros2_publisher)

    install(TARGETS
      ros2_publisher
      DESTINATION lib/${PROJECT_NAME})

endif()
