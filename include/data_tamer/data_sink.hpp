#pragma once

#include "data_tamer/contrib/SerializeMe.hpp"
#include "data_tamer/dictionary.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DataTamer {


using ActiveMask = std::vector<uint8_t>;
bool GetBit(const ActiveMask& mask, size_t index);
void SetBit(ActiveMask& mask, size_t index, bool val);

struct SnapshotHeader {
  /// Unique identifier of the dictionary
  std::size_t dictionary_hash;

  /// snapshot timestamp
  std::chrono::nanoseconds timestamp;

  /// Vector that tell us if an entry of the dictionary is
  /// active or not. It is basically an optimized vector
  /// of bools, where each byte contains 8 boolean flags.
  ActiveMask active_mask;
};

/**
 * @brief The DataSnapshot contains all the information passed by
 * LogChannel::takeSnapshot to a DataSync.
 *
 * Note: The vector contains:
 *
 * - The serialized SnapshotHeader in the first bytes
 * - The payload in the following bytes
 */
using DataSnapshot = std::vector<uint8_t>;


/**
 * @brief The DataSinkBase is the base class to use to create
 * your own DataSink
 */
class DataSinkBase {
 public:
  virtual ~DataSinkBase() = default;

  /**
   * @brief addChannel will register a dictionary into the sink.
   * That dictionary will be recognized by its hash.
   *
   * @param name        name of the channel
   * @param dictionary  dictionary.
   */
  virtual void addChannel(std::string const& name,
                          Dictionary const& dictionary) = 0;

  /**
   * @brief saveSnapshot is the method used by LogChannel to push new data.
   *
   * @param snapshot data to be pushed into the sink.
   * @return true if push was successful.
   */
  virtual bool storeSnapshot(const std::vector<uint8_t>& snapshot) = 0;
};

//--------------------------------------------------

inline bool GetBit(const ActiveMask &mask, size_t index)
{
  const uint8_t& byte = mask[index >> 3];
  return 0 != (byte & uint8_t(1 << (index % 8)));
}

inline void SetBit(ActiveMask &mask, size_t index, bool value)
{
  if(!value)
  {
    mask[index >> 3] &= uint8_t(~(1 << (index % 8)));
  }
  else {
    mask[index >> 3] |= uint8_t(1 << (index % 8));
  }
}

}  // namespace DataTamer

namespace SerializeMe
{

template <> struct Serializer<DataTamer::SnapshotHeader>
{
  template <class Operator> void operator()(DataTamer::SnapshotHeader& obj, Operator& op)
  {
    op(obj.dictionary_hash);
    op(obj.timestamp);
    op(obj.active_mask);
  }
};


}  // namespace SerializeMe
