#pragma once

#include "data_tamer/types.hpp"
#include <vector>
#include <ostream>

namespace DataTamer
{

struct Dictionary {
  struct Entry {
    std::string name;
    BasicType type;
  };
  std::vector<Entry> entries;
  uint64_t hash = 0;

  friend std::ostream& operator<<(std::ostream& os, const Dictionary& dict)
  {
    os << "hash: " << dict.hash << "\n";
    for(const auto& entry: dict.entries)
    {
      os << ToStr(entry.type) << ' ' << entry.name << '\n';
    }
    return os;
  }
};

}  // namespace DataTamer
