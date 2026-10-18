#pragma once

// Library-private: no installed header includes this file. Consumers see
// ChannelSharedState only as an incomplete type (LoggedValue holds a shared_ptr to it)
// and reach it through functions defined in channel.cpp: WriteTransaction for the write
// mutex, details::setSeriesEnabled() and isSeriesEnabled() for a LoggedValue's flag. So
// its layout, the flag word encoding and WriteMutex can change in a minor release.
#include "write_mutex.hpp"
#include "data_tamer_v2/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>

namespace DataTamer2
{

/**
 * @brief Append-only table of atomic words at stable addresses. One thread appends;
 * any thread may read existing words and size() without locks. Blocks are never moved
 * or freed before destruction, and size and block pointers are published with release.
 * Block i holds kFirstBlock << i words, 2^32 - 64 in all.
 */
class AtomicWordTable
{
public:
  static constexpr size_t kFirstBlock = 64;
  static constexpr size_t kBlocks = 26;

  AtomicWordTable() = default;
  AtomicWordTable(const AtomicWordTable&) = delete;
  AtomicWordTable& operator=(const AtomicWordTable&) = delete;
  ~AtomicWordTable()
  {
    for(auto& block : blocks_)
    {
      delete[] block.load(std::memory_order_relaxed);
    }
  }

  size_t size() const { return size_.load(std::memory_order_acquire); }

  /// Precondition: index < size() as observed by this thread.
  const std::atomic<uint32_t>& operator[](size_t index) const { return word(index); }
  std::atomic<uint32_t>& operator[](size_t index) { return word(index); }

  /// Control thread only. Throws std::length_error when the table is full.
  void push_back(uint32_t value)
  {
    const size_t index = size_.load(std::memory_order_relaxed);
    const auto [block, offset] = locate(index);
    if(block >= kBlocks)
    {
      throw std::length_error("too many registered series");
    }
    auto* words = blocks_[block].load(std::memory_order_relaxed);
    if(!words)
    {
      words = new std::atomic<uint32_t>[kFirstBlock << block];
      blocks_[block].store(words, std::memory_order_release);
    }
    words[offset].store(value, std::memory_order_relaxed);
    size_.store(index + 1, std::memory_order_release);
  }

private:
  std::atomic<uint32_t>& word(size_t index) const
  {
    const auto [block, offset] = locate(index);
    return blocks_[block].load(std::memory_order_acquire)[offset];
  }

  static std::pair<size_t, size_t> locate(size_t index)
  {
    size_t block = 0, base = 0, capacity = kFirstBlock;
    while(index >= base + capacity)
    {
      base += capacity;
      capacity <<= 1;
      ++block;
    }
    return { block, index - base };
  }

  std::array<std::atomic<std::atomic<uint32_t>*>, kBlocks> blocks_{};
  std::atomic<size_t> size_{ 0 };
};

/**
 * @brief State shared by a LogChannel and its LoggedValues (a shared_ptr in each), so
 * LoggedValue operations keep working after the channel is destroyed. Holds the write
 * mutex and one atomic flag word per series, appended by the control thread and read
 * lock-free by the snapshot thread and the writers.
 */
class ChannelSharedState
{
public:
  /// Held by writers inside a transaction, and by a snapshot from its pool slot to its
  /// last push.
  WriteMutex write_mutex;

  /// Set after every flag change; a snapshot clears it and rebuilds the channel's
  /// active mask. Keep every access seq_cst: it pairs with the channel's reader epoch.
  std::atomic<bool> mask_dirty{ true };

  /// Control thread only. Appends a registered, enabled series; returns its generation
  /// (always 1).
  uint32_t addSeries()
  {
    flags_.push_back(kFirstGeneration | kRegistered | kEnabled);
    return 1;
  }

  /// Generation of the registration in a slot; each re-registration bumps it.
  uint32_t generation(size_t index) const
  {
    return flags_[index].load(std::memory_order_seq_cst) >> kGenerationShift;
  }

  /// Whether `id` denotes the registration currently in its slot.
  bool isCurrent(const RegistrationID& id) const
  {
    return id.index_ < flags_.size() && generation(id.index_) == id.generation_;
  }

  size_t seriesCount() const { return flags_.size(); }

  bool isEnabled(size_t index) const
  {
    return (flags_[index].load(std::memory_order_seq_cst) & kFlagsMask) ==
           (kRegistered | kEnabled);
  }

  /// False for a stale or invalid id.
  bool isEnabled(const RegistrationID& id) const
  {
    if(id.index_ >= flags_.size())
    {
      return false;
    }
    const auto word = flags_[id.index_].load(std::memory_order_seq_cst);
    return (word >> kGenerationShift) == id.generation_ &&
           (word & kFlagsMask) == (kRegistered | kEnabled);
  }

  bool isRegistered(size_t index) const
  {
    return flags_[index].load(std::memory_order_seq_cst) & kRegistered;
  }

  /// Control thread only. Clears the registered flag; the caller then waits for the
  /// snapshot in progress before detaching the holder. The generation is kept: the id
  /// stays current (isEnabled() is false) until the slot is registered again.
  void setUnregistered(size_t index)
  {
    flags_[index].fetch_and(~uint32_t(kRegistered), std::memory_order_seq_cst);
    mask_dirty.store(true, std::memory_order_seq_cst);
  }

  static constexpr uint32_t kMaxGeneration = (uint32_t(1) << 24) - 1;

  /// Control thread only. False once the slot's generation counter is exhausted; check
  /// it before replacing the holder.
  bool canReregister(size_t index) const { return generation(index) < kMaxGeneration; }

  /// Control thread only. Publishes a replacement registration once its holder is
  /// initialized; returns the new generation, which makes older ids stale.
  /// Precondition: canReregister(index).
  uint32_t setReregistered(size_t index) noexcept
  {
    const uint32_t next = generation(index) + 1;
    flags_[index].store((next << kGenerationShift) | kRegistered | kEnabled,
                        std::memory_order_seq_cst);
    mask_dirty.store(true, std::memory_order_seq_cst);
    return next;
  }

  /// Lock-free. Changes the enabled bit only, with no staleness check.
  void setEnabled(size_t index, bool enable)
  {
    const auto old =
        enable ? flags_[index].fetch_or(kEnabled, std::memory_order_seq_cst) :
                 flags_[index].fetch_and(~uint32_t(kEnabled), std::memory_order_seq_cst);
    if(bool(old & kEnabled) != enable)
    {
      mask_dirty.store(true, std::memory_order_seq_cst);
    }
  }

  /// Lock-free and noexcept. Returns false, changing nothing, for a stale or invalid id:
  /// the generation check and the update are one atomic compare-exchange.
  bool setEnabled(const RegistrationID& id, bool enable) noexcept
  {
    if(id.index_ >= flags_.size())
    {
      return false;
    }
    auto& word = flags_[id.index_];
    auto current = word.load(std::memory_order_seq_cst);
    for(;;)
    {
      if((current >> kGenerationShift) != id.generation_)
      {
        return false;
      }
      const uint32_t desired =
          enable ? (current | kEnabled) : (current & ~uint32_t(kEnabled));
      if(desired == current)
      {
        return true;
      }
      if(word.compare_exchange_weak(current, desired, std::memory_order_seq_cst))
      {
        mask_dirty.store(true, std::memory_order_seq_cst);
        return true;
      }
    }
  }

  /// Whether the calling thread holds a transaction on this state.
  [[nodiscard]] bool inTransactionOnThisThread() const noexcept;

private:
  // One word per series: low byte holds the flags, the rest the generation. Accesses
  // are seq_cst: a reader that sees a registration also sees its initialized holder.
  static constexpr uint32_t kRegistered = 1;
  static constexpr uint32_t kEnabled = 2;
  static constexpr uint32_t kFlagsMask = 0xFF;
  static constexpr unsigned kGenerationShift = 8;
  static constexpr uint32_t kFirstGeneration = uint32_t(1) << kGenerationShift;
  static_assert(std::atomic<uint32_t>::is_always_lock_free, "series flags must be "
                                                            "lock-free");
  AtomicWordTable flags_;
};

}  // namespace DataTamer2
