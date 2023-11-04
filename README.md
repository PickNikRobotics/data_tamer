![Data Tamer](data_tamer_logo.png)

Spiritual successor of **pal_statistics**.

Stay tuned.


# Example

Preliminary example:

```cpp
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include <iostream>

std::chrono::microseconds UsecSinceEpoch()
{
  auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(since_epoch);
}

int main()
{
  using namespace DataTamer;

  // start defining one or more Sinks that must be added by default.
  // Dp ot BEFORE creating a channel.
  auto dummy_sink = std::make_shared<DummySync>();
  ChannelsRegistry::Global().addDefaultSink(dummy_sink);

  // Create (or get) a channel using the global registry (singleton)
  auto channel = ChannelsRegistry::Global().getChannel("chan");

  // If you don't want to use addDefaultSink, you can do:
  // channel->addDataSink(std::make_shared<DummySync>())

  // You can register any arithmetic value. You are responsible for their lifetime
  double value_real = 3.14;
  int value_int = 42;
  auto id1 = channel->registerValue("value_real", &value_real);
  auto id2 = channel->registerValue("value_int", &value_int);

  // If you prefer to use RAII, use this method instead
  // logged_real will disable itself when it goes out of scope.
  auto logged_float = channel->createLoggedValue<float>("real");

  // this is the way you store the current snapshot of the values
  channel->takeSnapshot( UsecSinceEpoch() );

  // You can disable a value like this
  channel->setEnabled(id1, false);
  // or
  logged_float->setEnabled(false);

  // The serialized data of the next snapshot will contain
  // only [value_int], i.e. [id2], since the other two are disabled
  channel->takeSnapshot( UsecSinceEpoch() );
}

```
