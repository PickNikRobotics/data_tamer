#pragma once

#include "data_tamer/data_sink.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"
#include <unordered_map>

namespace DataTamer
{

/**
 * @brief The DummySink does nothing, only counting the number of snapshots received.
 * Used mostly for testing and debugging.
 */
class DummySink : public DataSinkBase
{
public:
  std::unordered_map<uint64_t, Schema> schemas;
  std::unordered_map<uint64_t, std::string> schema_names;
  std::unordered_map<uint64_t, long> snapshots_count;
  Snapshot latest_snapshot;
  
  void addChannel(std::string const& name, Schema const& schema) override
  {
    schemas[schema.hash] = schema;
    schema_names[schema.hash] = name;
    snapshots_count[schema.hash] = 0;
  }

  bool storeSnapshot(const Snapshot& snapshot) override
  {
    latest_snapshot = snapshot;

    auto it = snapshots_count.find(snapshot.schema_hash);
    if( it != snapshots_count.end())
    {
      it->second++;
    }
    return true;
  }
};



}  // namespace DataTamer
