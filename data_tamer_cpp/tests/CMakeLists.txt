find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(datatamer_test
    dt_tests.cpp
    custom_types_tests.cpp
    parser_tests.cpp)
gtest_discover_tests(datatamer_test DISCOVERY_MODE PRE_TEST)

target_include_directories(datatamer_test
 PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

target_link_libraries(datatamer_test data_tamer GTest::gtest_main)

add_test(NAME datatamer_test COMMAND $<TARGET_FILE:datatamer_test>)
