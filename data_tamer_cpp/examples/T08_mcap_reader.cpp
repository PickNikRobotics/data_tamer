// T08 Reading an MCAP recording
// You will learn: decode every value of a recording, then count samples per time series.
// Run: ./T08_mcap_reader [file.mcap] (default: T01's recording), read the counts
#include "data_tamer_v2/data_tamer_parser.hpp"
#include "data_tamer_v2/mcap_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

int main(int argc, char** argv)
{
  const std::string filepath = argc > 1 ? argv[1] : "T01_joint_states.mcap";

  // samples per time series, per channel (the MCAP topic)
  std::map<std::string, std::map<std::string, size_t>> counts;
  try
  {
    // in log time order; the values themselves are not used here
    DataTamer2::ForEachValue(
        filepath,
        [&](const std::string& channel, uint64_t /*time*/, const std::string& name,
            const DataTamerParser::VarNumber& /*value*/) { ++counts[channel][name]; });
  }
  catch(const std::runtime_error& e)
  {
    // unreadable file, malformed schema or corrupt message: the message says which
    std::cerr << e.what() << std::endl;
    return 1;
  }

  for(const auto& [channel, series] : counts)
  {
    std::cout << channel << ":" << std::endl;
    for(const auto& [name, count] : series)
    {
      std::cout << "   " << name << ":" << count << std::endl;
    }
  }
  // An empty recording fails too, for scripts and ctest.
  return counts.empty() ? 1 : 0;
}
