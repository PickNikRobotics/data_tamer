# Examples are consumers: build them as plain C++17 to prove the public API needs
# no more. Under ROS 2, rclcpp may itself demand C++20 and CMake raises it.
function(CompileExample TARGET)
    add_executable(${TARGET} ${TARGET}.cpp)
    target_link_libraries(${TARGET} PRIVATE data_tamer)
    set_target_properties(${TARGET} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endfunction()

set(TUTORIALS
    T01_hello_mcap
    T02_custom_types
    T03_channels_and_registry
    T04_realtime_loop
    T05_threads
    T06_flight_recorder
    T07_custom_sink
    T08_mcap_reader)

foreach(TUTORIAL IN LISTS TUTORIALS)
    CompileExample(${TUTORIAL})
    # The parent also turns DATA_TAMER_BUILD_TESTS off when ROS BUILD_TESTING is off.
    if(DATA_TAMER_BUILD_TESTS)
        # The runner of tests/ (DATA_TAMER_TEST_WRAPPER, set by the tsan preset).
        if(DATA_TAMER_TEST_WRAPPER)
            set_property(TARGET ${TUTORIAL} PROPERTY CROSSCOMPILING_EMULATOR "${DATA_TAMER_TEST_WRAPPER}")
        endif()
        add_test(NAME ${TUTORIAL} COMMAND ${TUTORIAL})
        set_tests_properties(${TUTORIAL} PROPERTIES TIMEOUT 30)
    endif()
endforeach()

if(DATA_TAMER_BUILD_TESTS)
    # T08 reads the file T01 writes.
    set_tests_properties(T01_hello_mcap PROPERTIES FIXTURES_SETUP hello_mcap)
    set_tests_properties(T08_mcap_reader PROPERTIES FIXTURES_REQUIRED hello_mcap)
endif()

CompileExample(mcap_1m_per_sec)

if(DATA_TAMER_BUILD_ROS)
    CompileExample(ros2_publisher)
    if(DATA_TAMER_INSTALL)
        install(TARGETS ros2_publisher DESTINATION lib/${PROJECT_NAME})
    endif()
endif()
