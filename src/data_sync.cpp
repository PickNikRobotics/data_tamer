#include "data_tamer/data_sink.hpp"
#include "data_tamer/contrib/ringbuffer.hpp"

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
  jnk0le::Ringbuffer<std::vector<uint8_t>, 128, false, 32> queue;

  void threadLoop();
};

DataSinkBase::DataSinkBase(): _p(new Pimpl(this))
{}

DataSinkBase::~DataSinkBase()
{}

bool DataSinkBase::pushSnapshot(const std::vector<uint8_t> &snapshot)
{
  return _p->queue.insert(&snapshot);
}

void DataSinkBase::Pimpl::threadLoop()
{
  run = true;
  std::vector<uint8_t> snapshot;

  while(run)
  {
    if(queue.remove(&snapshot))
    {
      self->storeSnapshot(snapshot);
    }
    else {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
}

}  // namespace DataTamer
