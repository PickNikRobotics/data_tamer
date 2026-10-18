# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Modified for data_tamer from FindLZ4.cmake, as vendored with MCAP: it now prefers
# lz4's own CMake package and defines an imported target.

# Defines data_tamer::lz4, which the bundled MCAP implementation links. lz4's own
# package is used when there is one: LZ4::lz4_shared or LZ4::lz4_static (lz4 built
# with CMake, conan), or LZ4::lz4 (lz4 1.10). Otherwise the header and the library are
# searched for (Ubuntu 22.04 and 24.04 ship no CMake package for lz4). The shared
# library comes first: linked into a shared libdata_tamer, the static one would add the
# lz4 symbols to the exported ones.
if(NOT TARGET data_tamer::lz4)
  find_package(lz4 CONFIG QUIET)
  foreach(_candidate LZ4::lz4_shared LZ4::lz4 LZ4::lz4_static)
    if(TARGET ${_candidate})
      add_library(data_tamer::lz4 INTERFACE IMPORTED)
      target_link_libraries(data_tamer::lz4 INTERFACE ${_candidate})
      break()
    endif()
  endforeach()
  unset(_candidate)
endif()
if(NOT TARGET data_tamer::lz4)
  find_path(lz4_INCLUDE_DIR NAMES lz4.h)
  find_library(lz4_LIBRARY NAMES lz4 liblz4)
  mark_as_advanced(lz4_INCLUDE_DIR lz4_LIBRARY)
  if(lz4_INCLUDE_DIR AND lz4_LIBRARY)
    add_library(data_tamer::lz4 UNKNOWN IMPORTED)
    set_target_properties(data_tamer::lz4 PROPERTIES
      IMPORTED_LOCATION "${lz4_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${lz4_INCLUDE_DIR}")
  endif()
endif()

if(TARGET data_tamer::lz4)
  set(lz4_TARGET data_tamer::lz4)
endif()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(lz4 REQUIRED_VARS lz4_TARGET)
