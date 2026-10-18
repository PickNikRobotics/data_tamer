#pragma once

#include "data_tamer_v2/data_sink.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace DataTamer2
{

/// One pre-allocated snapshot plus its reference count (0 means free). Only a snapshot
/// holding the channel's write mutex moves refs from 0 to 1 and writes the snapshot,
/// while it holds the only reference; sinks read it and release. refs has its own cache
/// line, so sink traffic does not touch the snapshot data.
struct PoolSlot
{
  Snapshot snapshot;
  alignas(64) std::atomic<uint32_t> refs{ 0 };
};

/**
 * @brief Fixed-size pool of PoolSlot, shared by its LogChannel and every SnapshotRef
 * handed to a sink. Allocates only in the constructor.
 */
class SnapshotPool
{
public:
  /// The default size, when LogChannel::setPoolCapacity() was not called: the slots
  /// that fit in kDefaultBytes, at least kDefaultMinCapacity and at most
  /// kDefaultMaxCapacity (one second at 1 kHz). The cap bounds what a refused
  /// tryAcquire() costs: it reads every slot.
  static constexpr size_t kDefaultBytes = size_t(8) << 20;
  static constexpr size_t kDefaultMinCapacity = 64;
  static constexpr size_t kDefaultMaxCapacity = 1024;

  static constexpr size_t defaultCapacity(size_t payload_capacity, size_t mask_bytes)
  {
    const size_t slot_bytes = sizeof(PoolSlot) + payload_capacity + mask_bytes;
    return std::clamp(kDefaultBytes / slot_bytes, kDefaultMinCapacity,
                      kDefaultMaxCapacity);
  }

  SnapshotPool(size_t capacity, size_t payload_capacity, size_t mask_bytes)
    : capacity_(capacity), slots_(new PoolSlot[capacity])
  {
    for(size_t i = 0; i < capacity_; i++)
    {
      slots_[i].snapshot.payload.reserve(payload_capacity);
      slots_[i].snapshot.active_mask.resize(mask_bytes);
    }
  }

  SnapshotPool(const SnapshotPool&) = delete;
  SnapshotPool& operator=(const SnapshotPool&) = delete;

  /**
   * @brief Takes a free slot with one reference. The caller holds the channel's write
   * mutex; real-time safe.
   * @return the slot, or nullptr (counted in exhausted()) if every slot is in use.
   */
  [[nodiscard]] PoolSlot* tryAcquire()
  {
    for(size_t n = 0; n < capacity_; n++)
    {
      if(++scan_from_ == capacity_)
      {
        scan_from_ = 0;
      }
      PoolSlot& slot = slots_[scan_from_];
      // acquire: pairs with release(), so a sink's reads of the slot precede our writes.
      if(slot.refs.load(std::memory_order_acquire) == 0)
      {
        slot.refs.store(1, std::memory_order_relaxed);
        return &slot;
      }
    }
    exhausted_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  /// Wraps a reference already taken on `slot` in a SnapshotRef. Library internal.
  [[nodiscard]] static SnapshotRef adopt(std::shared_ptr<SnapshotPool> pool,
                                         PoolSlot* slot)
  {
    return SnapshotRef(std::move(pool), slot);
  }

  static void addRef(PoolSlot* slot)
  {
    slot->refs.fetch_add(1, std::memory_order_relaxed);
  }

  static void release(PoolSlot* slot)
  {
    slot->refs.fetch_sub(1, std::memory_order_release);
  }

  size_t capacity() const { return capacity_; }

  /// Number of slots in use. Diagnostic only: the count is racy.
  size_t inUse() const
  {
    size_t count = 0;
    for(size_t i = 0; i < capacity_; i++)
    {
      if(slots_[i].refs.load(std::memory_order_relaxed) != 0)
      {
        count++;
      }
    }
    return count;
  }

  uint64_t exhausted() const { return exhausted_.load(std::memory_order_relaxed); }

private:
  const size_t capacity_;
  std::unique_ptr<PoolSlot[]> slots_;
  size_t scan_from_ = 0;  // under the channel's write mutex
  std::atomic<uint64_t> exhausted_{ 0 };
};

}  // namespace DataTamer2
