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
  std::deque<std::atomic<bool>> enabled_;
};

}  // namespace DataTamer
