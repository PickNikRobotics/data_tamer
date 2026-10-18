# A consumer of an installed data_tamer. `conan create` builds it as the test package;
# CI and the manuals' install check configure it with CMAKE_PREFIX_PATH set to an
# install prefix.
cmake_minimum_required(VERSION 3.16)
project(test_package LANGUAGES CXX)

find_package(data_tamer 2 REQUIRED)

add_executable(test_package test_package.cpp)
target_link_libraries(test_package PRIVATE data_tamer::data_tamer)
