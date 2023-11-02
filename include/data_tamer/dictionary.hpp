#pragma once

#include "data_tamer/types.hpp"
#include <vector>

namespace DataTamer {

struct Dictionary {
  struct Entry {
    std::string name;
    BasicType type;
  };

  std::vector<Entry> ordered_entries;
  // unique identifier of the Dictionary
  std::size_t hash = 0;
};

}  // namespace DataTamer
