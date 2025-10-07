#include "data_tamer/data_sink.hpp"
#include "ConcurrentQueue/concurrentqueue.h"

#include <atomic>
#include <thread>

namespace DataTamer
{

struct DataSinkBase::Pimpl
{
  Pimpl(DataSinkBase* self)
  {
    run = true;

    thread = std::thread([this, self]() {
      Snapshot snapshot_copy;
      while(run)
      {
        while(queue.try_dequeue(snapshot_copy))
        {
          self->storeSnapshot(snapshot_copy);
        }
        // avoid busy loop
        std::this_thread::sleep_for(std::chrono::microseconds(250));
      }
    });
  }

  std::thread thread;
  std::atomic_bool run = true;
  std::atomic_bool accept_snapshots = true;
  moodycamel::ConcurrentQueue<Snapshot> queue;
};

DataSinkBase::DataSinkBase() : _p(new Pimpl(this)) {}

DataSinkBase::~DataSinkBase()
{
  stopThread();
}

bool DataSinkBase::pushSnapshot(const Snapshot& snapshot)
{
  if(_p->accept_snapshots)
  {
    return _p->queue.enqueue(snapshot);
  }
  else
  {
    return false;
  }
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
  Snapshot snapshot_copy;
  while(_p->queue.try_dequeue(snapshot_copy))
  {
    this->storeSnapshot(snapshot_copy);
  }
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
