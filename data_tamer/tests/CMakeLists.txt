find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(datatamer_test dt_tests.cpp)
gtest_discover_tests(datatamer_test DISCOVERY_MODE PRE_TEST)

target_include_directories(datatamer_test
 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)

target_link_libraries(datatamer_test data_tamer GTest::gtest_main)
