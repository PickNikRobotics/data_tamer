![Data Tamer](data_tamer_logo.png)

[![cmake Ubuntu](https://github.com/facontidavide/data_tamer/actions/workflows/cmake_ubuntu.yml/badge.svg)](https://github.com/facontidavide/data_tamer/actions/workflows/cmake_ubuntu.yml)
[![codecov](https://codecov.io/gh/facontidavide/data_tamer/graph/badge.svg?token=D0wtsntWds)](https://codecov.io/gh/facontidavide/data_tamer)

When we talk about "logging", most of the time we refer to human-readable
messages (strings) with different severity levels (INFO, ERROR, DEBUG, etc.), being displayed in 
the console or saved into a file.
 
**DataTamer** solves a different problem: it logs/traces numerical values over time and
takes periodic "snapshots" of these values, to later visualize them as **timeseries**.

As such, it is a great complement to [PlotJuggler](https://github.com/facontidavide/PlotJuggler),
the timeseries visualization tool (note: you will need PlotJuggler **3.8.2** or later).

**DataTamer** is your "fearless" C++ library to log numerical data because you can easily track 
hundreds or **thousands of variables**: even 1 million points per second should have a negligible CPU overhead.

Since all the values are aggregated in a single "snapshot", it is particularly 
suited to record data in a periodic loop (a very frequent use case in robotics applications).

Kudos to [pal_statistics](https://github.com/pal-robotics/pal_statistics), for inspiring this project.

## How it works

![architecture](concepts.png)

DataTamer can be used to monitor multiple variables in your applications.

**Channels** are used to take "snapshots" of a subset of variables at a given time.
If you want to record at different frequencies, you should use  different channels.

DataTamer will forward this data to 1 or multiple **Sinks**; 
a sink may save the information in a file (currently, we support [MCAP](https://mcap.dev/))
or publish it using an inter-process communication (for instance, a ROS2 publisher).

You can create your own sinks, if you want.

You can use [PlotJuggler](https://github.com/facontidavide/PlotJuggler) to
visualize your logs offline or in real-time.  


## Features

- **Serialization schema is created at run-time**: no need to do code generation.
- **Suitable for real-time applications**: very low latency (on the side of the callee).
- **Multi-sink architecture**: recorded data can be forwarded to multiple "backends". 
- **Very low serialization overhead**, in the order of 1 bit per traced value.
- The user can enable/disable traced variables at run-time.


## Limitations

- Traced variables can not be added (registered) once the recording starts.
- Focused on periodic recording. Not the best option for sporadic, asynchronous events.
- If you use `DataTamer::registerValue` you must be careful about the lifetime of the
object. If you prefer a safer RAII interface, use `DataTamer::createLoggedValue` instead.

# Example

```cpp
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"

int main()
{
  using namespace DataTamer;

  // Multiple channels can use this sink. 
  // Data will be saved in mylog.mcap
  auto mcap_sink = std::make_shared<MCAP>("mylog.mcap");

  // Create a channel
  auto channel = LogChannel::create("my_channel");
  channel->addDataSink(dummy_sink);

  // You can register any arithmetic value. You are responsible for their lifetime
  double value_real = 3.14;
  int value_int = 42;
  auto id1 = channel->registerValue("value_real", &value_real);
  auto id2 = channel->registerValue("value_int", &value_int);

  // If you prefer to use RAII, use this method instead
  // logged_real will disable itself when it goes out of scope.
  auto logged_float = channel->createLoggedValue<float>("my_real");

  // This is the way you store the current snapshot of the values
  channel->takeSnapshot();

  // You can disable (i.e., stop recording) a value
  channel->setEnabled(id1, false);
  // or
  logged_float->setEnabled(false);

  // The serialized data of the next snapshot will contain
  // only [value_int], i.e. [id2], since the other two were disabled
  channel->takeSnapshot();
}
```

# Compilation

### Compiling with ROS2

Just use colcon :)

## Compiling with Conan (not ROS2 support)

Note that th ROS2 publisher will **NOT** be built when using this method.

Assuming conan 2.x installed. From the source directory.

**Release**:

```
conan install . -s compiler.cppstd=gnu17 --build=missing -s build_type=Release
cmake -S . -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="build/Release/generators/conan_toolchain.cmake"
cmake --build build/Release/ --parallel
```

**Debug**:

```
conan install . -s compiler.cppstd=gnu17 --build=missing -s build_type=Debug
cmake -S . -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE="build/Debug/generators/conan_toolchain.cmake"
cmake --build build/Debug/ --parallel
```


