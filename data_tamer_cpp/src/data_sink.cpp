#include "data_tamer/data_sink.hpp"
#include "ConcurrentQueue/concurrentqueue.h"

#include <atomic>
#include <thread>

namespace DataTamer
{

// Number of producer threads (i.e. threads calling takeSnapshot on channels
// attached to this sink) for which the queue pre-allocates its blocks.
// Beyond this number, the first push from a new thread may allocate.
static constexpr size_t kExpectedProducerThreads = 4;

// Initial capacity of each slot's active_mask: one bit per registered value,
// so this covers channels with up to 512 values without any later growth.
static constexpr size_t kReservedMaskBytes = 64;

struct DataSinkBase::Pimpl
{
  Pimpl(DataSinkBase* self, size_t queue_size, size_t reserved_payload_bytes)
    : pool(queue_size)
    , free_slots(queue_size, 0, kExpectedProducerThreads)
    , ready_slots(queue_size, 0, kExpectedProducerThreads)
  {
    for(auto& slot : pool)
    {
      slot.payload.reserve(reserved_payload_bytes);
      slot.active_mask.reserve(kReservedMaskBytes);
      free_slots.enqueue(&slot);
    }

    run = true;
    thread = std::thread([this, self]() {
      while(run)
      {
        drainQueue(self);
        // avoid busy loop
        std::this_thread::sleep_for(std::chrono::microseconds(250));
      }
    });
  }

  void drainQueue(DataSinkBase* self)
  {
    Snapshot* slot = nullptr;
    while(ready_slots.try_dequeue(slot))
    {
      self->storeSnapshot(*slot);
      // capacity is sufficient by construction, but fall back to the
      // allocating version rather than leaking a slot.
      if(!free_slots.try_enqueue(slot))
      {
        free_slots.enqueue(slot);
      }
    }
  }

  std::thread thread;
  std::atomic_bool run = true;
  std::atomic_bool accept_snapshots = true;
  std::atomic<size_t> dropped_count = 0;

  // Pre-allocated snapshots. Pointers to these slots travel through the
  // two queues below; the slots themselves are never reallocated.
  std::vector<Snapshot> pool;
  moodycamel::ConcurrentQueue<Snapshot*> free_slots;
  moodycamel::ConcurrentQueue<Snapshot*> ready_slots;
};

DataSinkBase::DataSinkBase() : DataSinkBase(kDefaultQueueSize, 0) {}

DataSinkBase::DataSinkBase(size_t queue_size, size_t reserved_payload_bytes)
  : _p(new Pimpl(this, queue_size == 0 ? 1 : queue_size, reserved_payload_bytes))
{}

DataSinkBase::~DataSinkBase()
{
  stopThread();
}

bool DataSinkBase::pushSnapshot(const Snapshot& snapshot)
{
  if(!_p->accept_snapshots)
  {
    return false;
  }

  Snapshot* slot = nullptr;
  if(!_p->free_slots.try_dequeue(slot))
  {
    // queue is full: the consumer thread is not keeping up. Drop the newest.
    _p->dropped_count.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Copy into pre-allocated storage. std::vector::assign does not allocate
  // when the new size fits the existing capacity, so after the slot has seen
  // the largest payload once, this is a plain memcpy.
  slot->channel_name = snapshot.channel_name;
  slot->schema_hash = snapshot.schema_hash;
  slot->timestamp = snapshot.timestamp;
  slot->active_mask.assign(snapshot.active_mask.begin(), snapshot.active_mask.end());
  slot->payload.assign(snapshot.payload.begin(), snapshot.payload.end());

  if(!_p->ready_slots.try_enqueue(slot))
  {
    // Should not happen: both queues are sized for the whole pool.
    _p->free_slots.enqueue(slot);
    _p->dropped_count.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

size_t DataSinkBase::queueSize() const
{
  return _p->pool.size();
}

size_t DataSinkBase::droppedSnapshotsCount() const
{
  return _p->dropped_count.load(std::memory_order_relaxed);
}

void DataSinkBase::stopAcceptingSnapshots()
{
  _p->accept_snapshots = false;
}

void DataSinkBase::startAcceptingSnapshots()
{
  _p->accept_snapshots = true;
}

void DataSinkBase::processQueuedSnapshots()
{
  _p->drainQueue(this);
}

void DataSinkBase::stopThread()
{
  _p->run = false;
  if(_p->thread.joinable())
  {
    _p->thread.join();
  }
}

}  // namespace DataTamer
