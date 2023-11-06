#pragma once

#include "data_tamer/data_sink.hpp"
#include <unordered_map>

namespace DataTamer
{

/**
 * @brief The DummySync does nothing, only counting the number of snapshots received.
 * Used mostly for testing and debugging.
 */
class DummySync : public DataSinkBase
{
public:
  std::unordered_map<uint64_t, Dictionary> dictionaries;
  std::unordered_map<uint64_t, std::string> dictionary_names;
  std::unordered_map<uint64_t, long> snapshots_count;
  std::vector<uint8_t> latest_snapshot;

  void addChannel(std::string const& name, Dictionary const& dict) override
  {
    dictionaries[dict.hash] = dict;
    dictionary_names[dict.hash] = name;
    snapshots_count[dict.hash] = 0;
  }

  bool storeSnapshot(const std::vector<uint8_t>& snapshot) override
  {
    latest_snapshot = snapshot;
    uint64_t hash = 0;
    SerializeMe::SpanBytesConst span(snapshot.data(), snapshot.size());

    SerializeMe::DeserializeFromBuffer(span, hash);
    auto it = snapshots_count.find(hash);
    if( it != snapshots_count.end())
    {
      it->second++;
    }
    return true;
  }
};



}  // namespace DataTamer
