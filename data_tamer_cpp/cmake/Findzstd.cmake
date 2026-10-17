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
# Modified for data_tamer from FindZSTD.cmake, as vendored with MCAP: it now prefers
# zstd's own CMake package and defines an imported target.

# Defines data_tamer::zstd, which the bundled MCAP implementation links. zstd's own
# package is used when there is one: zstd::libzstd_shared or zstd::libzstd_static
# (Ubuntu 24.04 and later, conan), or zstd::libzstd (zstd 1.5.6 and later). Otherwise
# the header and the library are searched for (Ubuntu 22.04). The shared library comes
# first: linked into a shared libdata_tamer, the static one would add the zstd symbols
# to the exported ones (Debian points zstd::libzstd to it).
if(NOT TARGET data_tamer::zstd)
  find_package(zstd CONFIG QUIET)
  foreach(_candidate zstd::libzstd_shared zstd::libzstd zstd::libzstd_static)
    if(TARGET ${_candidate})
      add_library(data_tamer::zstd INTERFACE IMPORTED)
      target_link_libraries(data_tamer::zstd INTERFACE ${_candidate})
      break()
    endif()
  endforeach()
  unset(_candidate)
endif()
if(NOT TARGET data_tamer::zstd)
  find_path(zstd_INCLUDE_DIR NAMES zstd.h)
  find_library(zstd_LIBRARY NAMES zstd zstd_static)
  mark_as_advanced(zstd_INCLUDE_DIR zstd_LIBRARY)
  if(zstd_INCLUDE_DIR AND zstd_LIBRARY)
    add_library(data_tamer::zstd UNKNOWN IMPORTED)
    set_target_properties(data_tamer::zstd PROPERTIES
      IMPORTED_LOCATION "${zstd_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${zstd_INCLUDE_DIR}")
  endif()
endif()

if(TARGET data_tamer::zstd)
  set(zstd_TARGET data_tamer::zstd)
endif()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(zstd REQUIRED_VARS zstd_TARGET)
