# find_package(data_tamer) in a ROS 2 install, where the library is the ament package
# data_tamer_v2 (target data_tamer_v2::data_tamer). It provides data_tamer::data_tamer,
# the target name of every other install. Installed as
# <prefix>/share/data_tamer/cmake/data_tamerConfig.cmake, next to data_tamer_v2.
include(CMakeFindDependencyMacro)
get_filename_component(_data_tamer_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
find_dependency(data_tamer_v2 PATHS "${_data_tamer_prefix}" NO_DEFAULT_PATH)
unset(_data_tamer_prefix)

if(NOT TARGET data_tamer::data_tamer)
  add_library(data_tamer::data_tamer INTERFACE IMPORTED)
  set_target_properties(data_tamer::data_tamer PROPERTIES
    INTERFACE_LINK_LIBRARIES data_tamer_v2::data_tamer)
endif()
set(data_tamer_LIBRARY data_tamer::data_tamer)
