#include "data_tamer/data_sink.hpp"
#include "ConcurrentQueue/concurrentqueue.h"

namespace DataTamer
{

struct DataSinkBase::Pimpl
{
  Pimpl(DataSinkBase* base_class)
  {
    run = true;

    thread = std::thread(
        [this, base_class]() {
          while(run)
          {
            if(queue.try_dequeue(snapshot_copy))
            {
              base_class->storeSnapshot(snapshot_copy);
            }
            else {
              // avoid busy loop
              std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
          }
        });
  }

  ~Pimpl()
  {
    run = false;
    thread.join();
  }

  std::thread thread;
  std::atomic_bool run = true;
  moodycamel::ConcurrentQueue<Snapshot> queue;
  Snapshot snapshot_copy;
};

DataSinkBase::DataSinkBase(): _p(new Pimpl(this))
{}

DataSinkBase::~DataSinkBase()
{}

bool DataSinkBase::pushSnapshot(const Snapshot &snapshot)
{
  return _p->queue.enqueue(snapshot);
}

}  // namespace DataTamer
