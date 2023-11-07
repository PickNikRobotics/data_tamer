#include "data_tamer/data_sink.hpp"

namespace DataTamer
{

struct DataSinkBase::Pimpl
{
  Pimpl(DataSinkBase* base_class): self(base_class)
  {
    thread = std::thread([this]() { threadLoop(); });
  }
  ~Pimpl()
  {
    run = false;
    queue_cv.notify_one();
    thread.join();
  }

  DataSinkBase* self = nullptr;
  std::mutex mutex;
  std::thread thread;
  std::atomic_bool run = true;
  jnk0le::Ringbuffer<std::vector<uint8_t>, 128, false, 32> queue;
  std::condition_variable queue_cv;

  void threadLoop();
};

DataSinkBase::DataSinkBase(): _p(new Pimpl(this))
{}

DataSinkBase::~DataSinkBase()
{}

bool DataSinkBase::pushSnapshot(const std::vector<uint8_t> &snapshot)
{
  {
    std::lock_guard const lock(_p->mutex);
    if(_p->queue.isFull()) {
      return false;
    }
    _p->queue.insert(&snapshot);
  }
  _p->queue_cv.notify_one();
  return true;
}

void DataSinkBase::Pimpl::threadLoop()
{
  run = true;
  std::vector<uint8_t> snapshot;

  while(run)
  {
    std::unique_lock lk(mutex);
    queue_cv.wait(lk, [this]{
      return !queue.isEmpty() || !run;
    });

    if(!run)
    {
      break;
    }

    if(queue.remove(&snapshot))
    {
      lk.unlock();
      self->storeSnapshot(snapshot);
      lk.lock();
    }
  }
}

}  // namespace DataTamer
