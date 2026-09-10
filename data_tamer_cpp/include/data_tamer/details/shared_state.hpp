#pragma once

#include "data_tamer/details/write_mutex.hpp"
#include "data_tamer/types.hpp"

#include <atomic>
#include <cstddef>
#include <deque>

namespace DataTamer
{

/**
 * @brief State shared between a LogChannel and every LoggedValue registered
 * in it. Owned through shared_ptr by both, so writer-side operations
 * (set, setEnabled, transactions) never need the channel object and keep
 * working if the channel is destroyed first.
 *
 * Series flags are appended during registration (setup only, single
 * control thread) and read by the snapshot thread; std::deque keeps the
 * atomics at stable addresses while appending.
 */
class ChannelSharedState
{
public:
  /// The transaction lock: writers hold it for a scopedWrite(); the snapshot
  /// thread holds it for serialization. Priority-inheriting where available.
  WriteMutex write_mutex;

  /// Set (release) by any enable/disable; consumed (acq_rel exchange) by the
  /// snapshot thread when it rebuilds its private active mask.
  std::atomic<bool> mask_dirty{ true };

  void addSeries() { enabled_.emplace_back(true); }

  size_t seriesCount() const { return enabled_.size(); }

  bool isEnabled(size_t index) const
  {
    return enabled_[index].load(std::memory_order_relaxed);
  }

  /// Wait-free; callable from any thread, including inside a transaction.
  void setEnabled(size_t index, bool enable)
  {
    if(enabled_[index].exchange(enable, std::memory_order_relaxed) != enable)
    {
      mask_dirty.store(true, std::memory_order_release);
    }
  }

  void setEnabled(const RegistrationID& id, bool enable)
  {
    bool changed = false;
    for(size_t i = 0; i < id.fields_count; i++)
    {
      changed |= enabled_[id.first_index + i].exchange(enable, std::memory_order_relaxed) != enable;
    }
    if(changed)
    {
      mask_dirty.store(true, std::memory_order_release);
    }
  }

private:
  std::deque<std::atomic<bool>> enabled_;
};

}  // namespace DataTamer
