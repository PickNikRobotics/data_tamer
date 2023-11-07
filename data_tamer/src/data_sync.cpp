#include "data_tamer/data_sink.hpp"
#include "ConcurrentQueue/concurrentqueue.h"

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
    thread.join();
  }

  DataSinkBase* self = nullptr;
  std::thread thread;
  std::atomic_bool run = true;
  moodycamel::ConcurrentQueue<std::vector<uint8_t>> queue;

  void threadLoop();
};

DataSinkBase::DataSinkBase(): _p(new Pimpl(this))
{}

DataSinkBase::~DataSinkBase()
{}

bool DataSinkBase::pushSnapshot(const std::vector<uint8_t> &snapshot)
{
  return _p->queue.enqueue(snapshot);
}

void DataSinkBase::Pimpl::threadLoop()
{
  run = true;
  std::vector<uint8_t> snapshot;

  while(run)
  {
    if(queue.try_dequeue(snapshot))
    {
      self->storeSnapshot(snapshot);
    }
    else {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
}

}  // namespace DataTamer
