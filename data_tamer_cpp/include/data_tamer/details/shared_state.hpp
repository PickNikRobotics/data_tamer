#pragma once

#include "data_tamer/details/write_mutex.hpp"
#include "data_tamer/types.hpp"

#include <atomic>
#include <cassert>
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
  /**
   * @brief Scoped ownership of this state's write mutex.
   *
   * A nested transaction for the same state is a no-op. The thread-local
   * linked chain is allocation-free and has no nesting-depth limit.
   */
  class Transaction
  {
  public:
    explicit Transaction(ChannelSharedState& state) : state_(&state), previous_(active())
    {
      owns_ = !state.inTransactionOnThisThread();
      if(owns_)
      {
        state.write_mutex.lock();
      }
      active() = this;
    }

    ~Transaction()
    {
      assert(active() == this && "transactions are destroyed in LIFO order");
      active() = previous_;
      if(owns_)
      {
        state_->write_mutex.unlock();
      }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

  private:
    friend class ChannelSharedState;

    static Transaction*& active()
    {
      static thread_local Transaction* transaction = nullptr;
      return transaction;
    }

    ChannelSharedState* state_;
    Transaction* previous_;
    bool owns_ = false;
  };

  /// The transaction lock: writers hold it for a scopedWrite(); the snapshot
  /// thread holds it for serialization. Priority-inheriting where available.
  WriteMutex write_mutex;

  /// SC publication pairs with the snapshot's SC load/exchange and reader epoch.
  std::atomic<bool> mask_dirty{ true };

  /// Controller only. Returns the generation of the new slot (always 1).
  uint32_t addSeries()
  {
    flags_.emplace_back(kFirstGeneration | kRegistered | kEnabled);
    return 1;
  }

  /// Generation currently occupying a slot (bumped on every re-registration).
  uint32_t generation(size_t index) const
  {
    return flags_[index].load(std::memory_order_seq_cst) >> kGenerationShift;
  }

  /// Whether `id` denotes the registration currently occupying its slot.
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
      return false;
    const auto word = flags_[id.index_].load(std::memory_order_seq_cst);
    return (word >> kGenerationShift) == id.generation_ &&
           (word & kFlagsMask) == (kRegistered | kEnabled);
  }

  bool isRegistered(size_t index) const
  {
    return flags_[index].load(std::memory_order_seq_cst) & kRegistered;
  }

  /// Controller only: clear registration and wait for the reader before
  /// detaching the holder. The generation is kept, so the id stays valid for
  /// isEnabled() (false) until the slot is registered again.
  void setUnregistered(size_t index)
  {
    flags_[index].fetch_and(~uint32_t(kRegistered), std::memory_order_seq_cst);
    mask_dirty.store(true, std::memory_order_seq_cst);
  }

  /// Controller only: publish a replacement registration in a slot whose holder
  /// is already initialized. Returns the new generation; older ids are stale.
  uint32_t setReregistered(size_t index)
  {
    const auto previous = flags_[index].load(std::memory_order_seq_cst);
    const uint32_t generation = (previous >> kGenerationShift) + 1;
    flags_[index].store((generation << kGenerationShift) | kRegistered | kEnabled,
                        std::memory_order_seq_cst);
    mask_dirty.store(true, std::memory_order_seq_cst);
    return generation;
  }

  /// Lock-free; changes requested enablement only, never registration liveness.
  void setEnabled(size_t index, bool enable)
  {
    const auto old =
        enable ? flags_[index].fetch_or(kEnabled, std::memory_order_seq_cst) :
                 flags_[index].fetch_and(~uint32_t(kEnabled), std::memory_order_seq_cst);
    if(bool(old & kEnabled) != enable)
      mask_dirty.store(true, std::memory_order_seq_cst);
  }

  /// Lock-free. Returns false, changing nothing, if `id` is stale or invalid:
  /// the generation check and the flag update are one atomic exchange, so a
  /// concurrent re-registration cannot slip in between.
  bool setEnabled(const RegistrationID& id, bool enable)
  {
    if(id.index_ >= flags_.size())
      return false;
    auto& word = flags_[id.index_];
    auto current = word.load(std::memory_order_seq_cst);
    for(;;)
    {
      if((current >> kGenerationShift) != id.generation_)
        return false;
      const uint32_t desired =
          enable ? (current | kEnabled) : (current & ~uint32_t(kEnabled));
      if(desired == current)
        return true;
      if(word.compare_exchange_weak(current, desired, std::memory_order_seq_cst))
      {
        mask_dirty.store(true, std::memory_order_seq_cst);
        return true;
      }
    }
  }

  [[nodiscard]] bool inTransactionOnThisThread() const noexcept
  {
    for(auto* transaction = Transaction::active(); transaction;
        transaction = transaction->previous_)
    {
      if(transaction->state_ == this)
      {
        return true;
      }
    }
    return false;
  }

private:
  // One word per series: low byte holds the flags, the rest the generation.
  static constexpr uint32_t kRegistered = 1;
  static constexpr uint32_t kEnabled = 2;
  static constexpr uint32_t kFlagsMask = 0xFF;
  static constexpr unsigned kGenerationShift = 8;
  static constexpr uint32_t kFirstGeneration = uint32_t(1) << kGenerationShift;
  static_assert(std::atomic<uint32_t>::is_always_lock_free, "series flags must be "
                                                            "lock-free");
  std::deque<std::atomic<uint32_t>> flags_;
};

}  // namespace DataTamer
