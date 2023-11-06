![Data Tamer](data_tamer_logo.png)

**DataTamer** is a spiritual successor of [pal_statistics](https://github.com/pal-robotics/pal_statistics).

Its purpose is to log **numerical** values in your application and export them into a
format that allow the user to visualize them as timeseries, 
for instance in [PlotJuggler](https://github.com/facontidavide/PlotJuggler).

All the values are aggregated in a single "snapshot", for this reason, it is particularly 
suited to record data in a periodic loop (very frequent in robotics applications).

## Features

- Schemaless serialization or, to be more precise, schema created at run-time.
- Very low-latency on the callee side: actual recording is done in a separate thread.
- Serialization has almost no overhead; the schema is saved/published separatly.
- The user can enable/disable logged variables at run-time.
- Direct [MCAP](https://mcap.dev/) recording.
- ROS2 publisher (comping soon). 


# Example


```cpp
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include <iostream>

int main()
{
  using namespace DataTamer;

  // start defining one or more Sinks that must be added by default.
  // Do this BEFORE creating a channel.
  auto dummy_sink = std::make_shared<DummySync>();
  ChannelsRegistry::Global().addDefaultSink(dummy_sink);

  // Create a channel (or get an existing one) using the 
  // global registry (singleton-like interface)
  auto channel = ChannelsRegistry::Global().getChannel("my_channel");

  // If you don't want to use addDefaultSink, you can do it manually:
  // channel->addDataSink(std::make_shared<DummySync>())

  // You can register any arithmetic value. You are responsible for their lifetime
  double value_real = 3.14;
  int value_int = 42;
  auto id1 = channel->registerValue("value_real", &value_real);
  auto id2 = channel->registerValue("value_int", &value_int);

  // If you prefer to use RAII, use this method instead
  // logged_real will disable itself when it goes out of scope.
  auto logged_float = channel->createLoggedValue<float>("my_real");

  // this is the way you store the current snapshot of the values
  channel->takeSnapshot( UsecSinceEpoch() );

  // You can disable (i.e., stop recording) a value like this
  channel->setEnabled(id1, false);
  // or
  logged_float->setEnabled(false);

  // The serialized data of the next snapshot will contain
  // only [value_int], i.e. [id2], since the other two were disabled
  channel->takeSnapshot( UsecSinceEpoch() );
}
```
