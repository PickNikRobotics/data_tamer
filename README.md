![Data Tamer](data_tamer_logo.png)

[![cmake Ubuntu](https://github.com/facontidavide/data_tamer/actions/workflows/cmake_ubuntu.yml/badge.svg)](https://github.com/facontidavide/data_tamer/actions/workflows/cmake_ubuntu.yml)
[![ros2 humble](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-humble.yml/badge.svg)](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-humble.yml)
[![ros2 jazzy](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-jazzy.yml/badge.svg)](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-jazzy.yml)
[![ros2 lyrical](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-lyrical.yml/badge.svg)](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-lyrical.yml)
[![ros2 rolling](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-rolling.yml/badge.svg)](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-rolling.yml)
[![codecov](https://codecov.io/gh/facontidavide/data_tamer/graph/badge.svg?token=D0wtsntWds)](https://codecov.io/gh/facontidavide/data_tamer)

**DataTamer** is a library to log/trace numerical variables over time and
takes periodic "snapshots" of their values, to later visualize them as **timeseries**.

It works great with [PlotJuggler](https://github.com/facontidavide/PlotJuggler),
the timeseries visualization tool (note: you will need PlotJuggler **3.8.2** or later).

**DataTamer** is "fearless data logger" because you can record hundreds or **thousands of variables**:
even 1 million points per second should have a fairly small CPU overhead.

Since all the values are aggregated in a single "snapshot", it is usually meant to
record data in a periodic loop (a very frequent use case, in robotics applications).

Kudos to [pal_statistics](https://github.com/pal-robotics/pal_statistics), for inspiring this project.

## How it works

![architecture](concepts.png)

DataTamer can be used to monitor multiple variables in your applications.

**Channels** are used to take "snapshots" of a subset of variables at a given time.
If you want to record at different frequencies, you can use different channels.

DataTamer will forward the collected data to 1 or multiple **sinks**;
a sink may save the information immediately in a file (currently, we support [MCAP](https://mcap.dev/))
or publish it using an inter-process communication, for instance, a ROS2 publisher.

You can easily create your own, specialized sinks.

Use [PlotJuggler](https://github.com/facontidavide/PlotJuggler) to
visualize your logs offline or in real-time.

## Features

- **Serialization schema is created at run-time**: no need to do code generation.
- **Suitable for real-time applications**: very low latency (on the side of the callee).
- **Multi-sink architecture**: recorded data can be forwarded to multiple "backends".
- **Very low serialization overhead**, in the order of 1 bit per traced value.
- The user can enable/disable traced variables at run-time.

## Limitations

- New traced variables can not be added once the first `takeSnapshot()` attempt
  freezes the schema. A previously removed name may be re-registered after that
  point only with its compatible original type, reusing its schema slot.
- Focused on periodic recording. Not the best option for sporadic, asynchronous events.
- If you use `DataTamer::registerValue` you must be careful about the lifetime of the
object. If you prefer a safer RAII interface, use `DataTamer::createLoggedValue` instead.

## Real-time snapshot contract

Exactly one thread per channel may call `takeSnapshot()`. Its first attempt
freezes the schema. Successful setup registers it with attached sinks, creates
the snapshot pool and reserves every slot; setup may lock, allocate and throw,
even when no sink is attached. Configure it beforehand when the defaults are
unsuitable:

```cpp
channel->setPoolCapacity(64);       // retained or in-flight snapshots
channel->setPayloadCapacity(16 * 1024); // reservation hint, not a byte ceiling
channel->setStrictMode(true);       // drop instead of growing a slot
channel->takeSnapshot();            // freezes the schema and capacities
```

The default pool has 64 slots. Each slot initially reserves at least
`max(payload_hint, 2 * initial_payload_size, 256)` bytes, so the hint is a
minimum. The vector's actual capacity decides whether a later payload fits.
With strict mode disabled, an acquired slot grows to twice the required size
and increments `payloadReallocations()`; the other slots grow only when used.
With strict mode enabled, a slot that is still too small is released, the
snapshot is dropped, and `droppedOversize()` increments; later input cannot grow
that slot. Capacity already gained by a slot is retained across strict-mode
changes. Strict mode therefore fixes per-slot payload capacity. Warming alone
does not bound non-strict growth: a later larger payload can grow the acquired
slot again. In non-strict mode, byte usage is bounded only when payload sizes
have a known upper bound that every slot can hold.

After successful setup, `takeSnapshot()` allocates no library-owned frontend
storage while payloads fit their slots. It serializes directly into one pool
slot and publishes references to at most eight attached sinks. It may wait for
the channel `WriteMutex`; keep transactions, non-scalar proxy guards and custom
serializers short. User serializers may allocate or throw, and the allocator,
OS scheduler and serializer work prevent a universal no-throw or hard-deadline
guarantee. A pinned same-machine pair for two sinks and two transaction writers
also regressed at the median, while its observed maximum improved:

| Runtime | p50 (ns) | max (ns) |
|---|---:|---:|
| Plan 3 | 20,478 | 214,852 |
| Final | 32,896 | 181,903 |

This one CPUs 0–5 pair is context, not a latency bound. The unpinned main
matrix also has median regressions; see the
[Plan 4 measurements](docs/benchmarks/2026-09-plan4.md) for the full results
and allocation evidence.

Scalar `LoggedValue::set()` and `get()` are wait-free relaxed atomic operations.
Each scalar is read without tearing, but unrelated scalar writes may land in
different snapshots. Use `scopedWrite()` for an all-or-nothing group; non-scalar
accessors take that same mutex automatically.

Backpressure is reported at two levels. `droppedSnapshots(sink)` counts queue
failures for one current channel/sink attachment. `poolExhausted()` counts a
global channel failure before serialization, so every sink misses that attempt;
retaining callback snapshots can cause this starvation. `stats()` returns a
plain `uint64_t` point-in-time copy of the write-lock, pool, growth and oversize
counters; the counters it reads are relaxed atomics.
`droppedSnapshots(sink)` instead takes the channel control mutex, and
`DataSinkBase::storeErrors()` counts exceptions thrown later by sink callbacks.
Sink queue capacities are block-rounded minima (default 1024), not exact limits.

`setEnabled()` remains lock-free during logging. Registration, unregistration,
sink changes, capacity setup and channel destruction are control operations:
call them outside `scopedWrite()`/pointer guards and serializer callbacks; they
may allocate or wait for an active snapshot. Calls that use the channel object
still need ordinary external lifetime synchronization. Re-registering a
previously removed name with a compatible type reuses its schema slot even
after freeze. A `RegistrationID` identifies that slot, not a generation, so an
older same-name ID also refers to the replacement.

# Examples

## Basic example

```cpp
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"

int main()
{
  // Multiple channels can use this sink. Data will be saved in mylog.mcap
  auto mcap_sink = std::make_shared<DataTamer::MCAPSink>("mylog.mcap");

  // Create a channel and attach a sink. A channel can have multiple sinks
  auto channel = DataTamer::LogChannel::create("my_channel");
  channel->addDataSink(mcap_sink);

  // You can register any arithmetic value. You are responsible for their lifetime!
  double value_real = 3.14;
  int value_int = 42;
  auto id1 = channel->registerValue("value_real", &value_real);
  auto id2 = channel->registerValue("value_int", &value_int);

  // If you prefer to use RAII, use this method instead
  // logged_real will unregister itself when it goes out of scope.
  auto logged_real = channel->createLoggedValue<float>("my_real");

  // Store the current value of all the registered values
  channel->takeSnapshot();

  // You can disable (i.e., stop recording) a value like this
  channel->setEnabled(id1, false);
  // or, in the case of a LoggedValue
  logged_real->setEnabled(false);

  // The next snapshot will contain only [value_int], i.e. [id2],
  // since the other two were disabled
  channel->takeSnapshot();
}
```
Scalar `LoggedValue::set()` and `get()` use relaxed atomics. Each value is
read without tearing, but separate writes can appear in different snapshots.
To capture several updates together, use one logged struct or a transaction:

```cpp
{
  auto tx = channel->scopedWrite();
  logged_real->set(3.2f);
  value_int = 43;  // a raw registered value uses the same write mutex
}
channel->takeSnapshot();
```

Non-scalar `set()`/`get()` lock automatically and can be called inside
`scopedWrite()`. Keep transactions short and take snapshots after releasing
them. Non-scalar pointer proxies also hold the write mutex; release them before
starting a transaction or taking a snapshot.

## How to register custom types

Containers such as `std::vector` and `std::array` are supported out of the box.
You can also register a custom type, as shown in the example below.

```cpp
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "data_tamer/custom_types.hpp"

// This is your custom type
namespace MyNamespace
{
struct Point3D
{
  double x;
  double y;
  double z;
};
} // end namespace MyNamespace

// You must implement the function TypeDefinition in the same namespace as Point3D
namespace MyNamespace
{
template <typename AddField>
std::string_view TypeDefinition(Point3D& point, AddField& add) {
  add("x", &point.x);
  add("y", &point.y);
  add("z", &point.z);
  return "Point3D";
}
} // end namespace MyNamespace

int main()
{
  auto channel = DataTamer::LogChannel::create("my_channel");
  channel->addDataSink(std::make_shared<DataTamer::MCAPSink>("mylog.mcap"));

  // Array/vectors are supported natively
  std::vector<double> values = {1, 2, 3, 4};
  channel->registerValue("values", &values);

  // Requires the implementation of DataTamer::TypeDefinition<Point3D>
  Point3D position = {0.1, -0.2, 0.3};
  channel->registerValue("position", &position);

  // save the data as usual ...
  channel->takeSnapshot();
}
```

# Compilation

## Compiling with ROS2

Just use colcon :)

## Compiling with Conan (not ROS2 support)

Note that the ROS2 publisher will **NOT** be built when using this method.

Assuming conan 2.x installed. From the source directory.

**Release**:

```
conan install . -s compiler.cppstd=gnu17 --build=missing -s build_type=Release
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="build/Release/generators/conan_toolchain.cmake"
cmake --build build/Release --parallel
```

**Debug**:

```
conan install . -s compiler.cppstd=gnu17 --build=missing -s build_type=Debug
cmake -S . -B build/Debug -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE="build/Debug/generators/conan_toolchain.cmake"
cmake --build build/Debug --parallel
```

# How to deserialize data recorded with DataTamer

I will write more extensively about the serialization format used by DataTamer, but for the time being I
created a single header file without external dependencies that you can just copy into your project:
[data_tamer_parser.hpp](data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp)

You can see how it is used in this example: [mcap_reader](data_tamer_cpp/examples/mcap_reader.cpp)
