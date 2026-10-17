# Migrating from 1.x to 2.0

This guide is for code written against data_tamer 1.x (1.0.4 and earlier). 2.0 breaks both
source and binary compatibility. Most changes show up as compile errors, and the
[quick reference](#quick-reference) maps each one to its replacement. A few changes
compile but behave differently: read [Behaviour changes](#behaviour-changes) even when
your code builds. [Reading the data](#reading-the-data) covers the readers, PlotJuggler
included.

- [Steps](#steps)
- [Quick reference](#quick-reference)
- [Sinks](#sinks)
- [Taking snapshots](#taking-snapshots)
- [Writing values from other threads](#writing-values-from-other-threads)
- [Registering values](#registering-values)
- [Behaviour changes](#behaviour-changes)
- [Reading the data](#reading-the-data)
- [Building and linking](#building-and-linking)

## Steps

1. Rebuild every binary that links libdata_tamer. Don't mix 1.x and 2.0 binaries.
2. Change every `#include "data_tamer/..."` to `#include "data_tamer_v2/..."`, and
   `data_tamer_parser/data_tamer_parser.hpp` to `data_tamer_v2/data_tamer_parser.hpp`.
3. Create sinks with their `create()` function, and port custom sinks to `DataSink`.
4. Replace `if(!channel->takeSnapshot())` with a check of the `SnapshotResult`.
5. In other threads, replace `writeMutex()` with `scopedWrite()`.
6. Stop the sinks before the program exits: `ChannelsRegistry::Global().stopAll()` or
   `worker->stop()`.
7. Check the [behaviour changes](#behaviour-changes): MCAP files now roll over, the
   pool now drops snapshots, and the ROS 2 QoS changed.
8. Update every reader of the recorded data: 1.x parsers can't read 2.0 files.

## Quick reference

| 1.x | 2.0 |
|---|---|
| `#include "data_tamer/channel.hpp"` (every header) | `#include "data_tamer_v2/channel.hpp"` |
| `#include "data_tamer_parser/data_tamer_parser.hpp"` | `#include "data_tamer_v2/data_tamer_parser.hpp"` |
| `std::make_shared<MCAPSink>(path)` | `MCAPSink::create(path)`, a `std::shared_ptr<SinkWorker>` |
| `std::make_shared<ROS2PublisherSink>(node, prefix)` | `ROS2PublisherSink::create(node, prefix)` |
| `std::make_shared<DummySink>()` | `DummySink::create()` |
| `sink->setMaxTimeBeforeReset(t)` and other sink methods | `worker->as<MCAPSink>().setMaxTimeBeforeReset(t)` |
| `MCAPSink::finishQueueAndStop()` | `worker->stop()`, which also closes the file |
| `class MySink : public DataSinkBase` | `class MySink : public DataSink`, see [custom sinks](#custom-sinks) |
| `if(!channel->takeSnapshot())` | `if(channel->takeSnapshot() != SnapshotResult::ok)` |
| `channel->getActiveFlags()` | removed: read `active_mask` of a delivered snapshot |
| `std::lock_guard lock(channel->writeMutex())` | `auto tx = channel->scopedWrite()` |
| `Mutex`, `DataTamer::Mutex` | removed |
| `scalar->getMutablePtr()`, `getConstPtr()` | `scalar->set(v)`, `scalar->get()` |
| `value->getLockedPtr()` | `value->getMutablePtr()` (structs and vectors only) |
| `if(auto ref = value->getMutablePtr())` | `auto ref = value->getMutablePtr();`: the guards have no `operator bool` |
| `value->set(v, auto_enable)` | `value->set(v)`, which never re-enables: call `setEnabled(true)` |
| `ptr.mutex()` on a `MutablePtr` / `ConstPtr` | removed; the guards are not movable |
| `id.first_index`, `id.fields_count`, `id += other` | removed: `RegistrationID` is opaque |
| `Snapshot::channel_name` | `Schema::channel_name`, matched by `schema_hash` |
| `DataSnapshot` | removed |
| `DummySink::latest_snapshot`, `schemas`, ... | `latestSnapshot()`, `schema(hash)`, ... |
| a name converted implicitly to `CustomSerializerT<T>` | `CustomSerializerT<T>("name")`: the constructor is `explicit` |
| `AddFieldToHash()` | `SchemaTextHash()`, `ComputeSchemaHash()` |
| `DataTamerParser::BuilSchemaFromText()` | `BuildSchemaFromText()` (the old name is deprecated) |
| `ParseSnapshot(schema, view, on_number, on_custom)` | `ParseSnapshot(schema, view, on_number)` |

`data_tamer_v2/data_tamer.hpp` includes the whole channel API, as `data_tamer/data_tamer.hpp`
did in 1.x. The new folder name lets 1.x and 2.x headers be installed side by side.

## Sinks

### Built-in sinks

A sink no longer owns a thread. A `SinkWorker` owns the sink and the thread that delivers
snapshots to it, and channels attach workers. Each built-in sink has a `create()` that
returns a ready-to-attach worker:

```cpp
// 1.x
auto mcap = std::make_shared<DataTamer::MCAPSink>("run.mcap");
mcap->setMaxTimeBeforeReset(std::chrono::seconds(60));
DataTamer::ChannelsRegistry::Global().addDefaultSink(mcap);

// 2.0
auto worker = DataTamer::MCAPSink::create("run.mcap");
worker->as<DataTamer::MCAPSink>().setMaxTimeBeforeReset(std::chrono::seconds(60));
DataTamer::ChannelsRegistry::Global().addDefaultSink(worker);
```

`as<T>()` throws `std::bad_cast` if the sink is not a `T`. `addDefaultSink()` also
attaches the sink to channels that exist already, so it no longer has to come before
`getChannel()`. `ROS2PublisherSink::create()` takes an optional `ROS2PublisherOptions`
third argument. A channel holds at most 8 sinks, and so does the registry as default
sinks: a ninth throws `std::runtime_error`.

To stop a sink, call `worker->stop()`, or `ChannelsRegistry::Global().stopAll()` for every
sink of the registry. `stop()` delivers what is queued, then finishes the sink: `MCAPSink`
closes its file. `worker->start()` resumes, and `MCAPSink` continues in its next numbered
file. `MCAPSink::restartRecording()` still opens a new file, but it no longer resumes a
stopped worker: call `worker->start()` after it.

### Custom sinks

`DataSinkBase` is gone. Derive from `DataSink`, implement its protected callbacks, and wrap
the sink in a `SinkWorker`:

```cpp
// 1.x
class CsvSink : public DataTamer::DataSinkBase
{
public:
  ~CsvSink() override { stopThread(); }
  void addChannel(const std::string& name, const DataTamer::Schema& schema) override
  {
    names_[schema.hash] = name;
  }
  bool storeSnapshot(const DataTamer::Snapshot& snapshot) override
  {
    return write(names_[snapshot.schema_hash], snapshot.payload);
  }

private:
  std::unordered_map<uint64_t, std::string> names_;
  bool write(const std::string& channel, const DataTamer::PayloadVector& payload);
};
auto sink = std::make_shared<CsvSink>();

// 2.0
#include "data_tamer_v2/data_sink.hpp"
#include "data_tamer_v2/types.hpp"  // Schema: data_sink.hpp only declares it
#include <stdexcept>
#include <unordered_map>

class CsvSink : public DataTamer::DataSink
{
protected:
  void onSchema(const DataTamer::Schema& schema) override
  {
    names_[schema.hash] = schema.channel_name;
  }
  void onSnapshot(const DataTamer::SnapshotRef& snapshot) override
  {
    if(!write(names_[snapshot->schema_hash], snapshot->payload))
    {
      throw std::runtime_error("CsvSink: write failed");  // counted in errors()
    }
  }

private:
  std::unordered_map<uint64_t, std::string> names_;
  bool write(const std::string& channel, const DataTamer::PayloadVector& payload);
};
auto worker = DataTamer::SinkWorker::create<CsvSink>();
```

| 1.x | 2.0 |
|---|---|
| `addChannel(name, schema)`, public | `onSchema(schema)`, protected; the name is `schema.channel_name` |
| `bool storeSnapshot(const Snapshot&)` | `void onSnapshot(const SnapshotRef&)`; throw to report a failure |
| return `false` from `storeSnapshot()` | throw: `worker->errors()` and `lastError()` count and keep it |
| override `pushSnapshot()` | removed: the channel pushes into the worker's queue |
| `stopThread()` in the destructor | delete it: the worker stops before it destroys the sink |
| `stopAcceptingSnapshots()`, `processQueuedSnapshots()`, `startAcceptingSnapshots()` | `worker->stop()`, `worker->drain()`, `worker->start()` |
| close files in the destructor | override `onStop()`, and `onStart()` to reopen them |
| `Snapshot::schema_hash` was `size_t` | it is `uint64_t` |

A `SnapshotRef` holds a slot of the channel's snapshot pool, which all sinks of the
channel share. Return from `onSnapshot()` quickly. For slow work, copy the snapshot
(`DataTamer::Snapshot copy = *snapshot;`) and hand the copy to a thread of your own.
Never call a function that changes the channel (registration, sinks, `startLogging()`)
from a callback: it deadlocks.

`DummySink` has accessors instead of public members: `latestSnapshot()`,
`latestPayloadSize()`, `latestActiveMask()`, `snapshotsCount(hash)`, `schemasCount()`,
`firstSchemaHash()` and `schema(hash)`. `schema_names` is gone: use
`schema(hash).channel_name`. In tests, call `worker->drain()` before reading them,
instead of sleeping.

## Taking snapshots

`takeSnapshot()` returns a `SnapshotResult` instead of a `bool`. `ok` means every sink got
the snapshot; the [user manual](USER_MANUAL.md#snapshots-are-missing-from-the-recording)
lists the other values. A plain `channel->takeSnapshot();` still compiles, and
`channel->stats()` counts every outcome, but `if(!channel->takeSnapshot())` doesn't:

```cpp
if(channel->takeSnapshot() != DataTamer::SnapshotResult::ok)
{
  // count it; channel->stats() has the totals
}
```

`getActiveFlags()` is removed. A sink reads the mask of each snapshot from
`Snapshot::active_mask`.

2.0 adds `startLogging()`, which freezes the schema, allocates the snapshot pool and sends
the schema to the sinks. The first `takeSnapshot()` calls it for you, so 1.x code needs no
change. Call it yourself before the loop when the first iteration must be fast, or with
`ROS2PublisherSink`, which publishes the schemas from `startLogging()`. A real-time loop
calls the new `tryTakeSnapshot()`, which never blocks or allocates, and requires
`startLogging()` first (see [Real-time use](USER_MANUAL.md#real-time-use)).

## Writing values from other threads

The channel's write mutex is no longer exposed. A thread other than the snapshot thread
writes registered variables inside a transaction:

```cpp
// 1.x
{
  std::lock_guard lock(channel->writeMutex());
  position = p;
  velocity = v;
}

// 2.0
{
  auto tx = channel->scopedWrite();
  position = p;
  velocity = v;
}
```

`scopedWrite()` nests, and a `LoggedValue` set inside it doesn't lock again. A snapshot
taken by a thread that holds a transaction, or a `LoggedValue` guard, returns `blocked`.

`LoggedValue` changed in four ways:

- Scalars (numbers, `bool`, `char`, enums) are `std::atomic`. Use `set()` and `get()`:
  `getMutablePtr()` and `getConstPtr()` on a scalar fail with "scalar LoggedValues are
  atomic: use set()/get()".
- For structs and vectors, `getMutablePtr()` and `getConstPtr()` remain. The guards are not
  movable and have no `operator bool`. `getLockedPtr()` is removed.
- `set(value)` lost its `auto_enable` parameter and never re-enables a disabled value.
  Call `setEnabled(true)`.
- A `LoggedValue` can't be moved or default-constructed. Keep the `std::shared_ptr` that
  `createLoggedValue()` returns.

`set(get() + x)` from two threads is not one atomic step. For a read-modify-write from
several threads, do it inside `scopedWrite()`.

## Registering values

- `RegistrationID` is an opaque handle. To enable or disable a group of values, keep one
  id per value, in a `std::vector<RegistrationID>` for example, instead of adding ids
  together. A default-constructed id is invalid, and so is an id whose name was
  unregistered and registered again: `setEnabled()` and `unregister()` throw
  `std::invalid_argument`, and the new `trySetEnabled()` returns false.
- Names are checked more strictly. 1.x rejected only spaces. 2.0 also rejects empty names,
  tabs, line breaks and control characters, in value names and in the names of custom
  types and their fields. A channel name may contain spaces, but it can't be empty or
  contain control characters. `DataTamer::JoinNames()` (`data_tamer_v2/names.hpp`) builds
  `a/b/c` names without empty components.
- `long long`, `unsigned long long`, `wchar_t`, `char16_t` and `char32_t` are recorded as
  integers of their size. 1.x recorded them as `other`, which no reader decodes.
  `long double` is a compile error ("has no wire type").
- Two C++ types with the same custom type name in one channel make `registerValue()`
  throw. In 1.x the second one was serialized with the first one's serializer.
- `CustomTypeName<T>::get()` is no longer `constexpr`, and `Schema::custom_types` and
  `custom_schemas` are `std::map` (they were `std::unordered_map`).
- New: `registerValue()` accepts a `const std::atomic<T>*`, and
  `DataTamer::TypeDefinitionTrait<T>` describes a type whose namespace you can't reopen
  (see [custom types](USER_MANUAL.md#custom-types)). Existing `TypeDefinition()`
  functions keep working.
- `getPointerType()` is removed.

As in 1.x, registering a new name after the first snapshot throws.

## Behaviour changes

These changes compile without a warning.

| Area | 1.x | 2.0 | To get the 1.x behaviour |
|---|---|---|---|
| MCAP reset every 600 s | truncated the file, losing everything recorded before | continues in a new numbered file; disk use is unbounded | `setCreateNewFileOnReset(false)` |
| Rollover file names | `run.mcap_1` | `run_1.mcap`; existing files are skipped, not overwritten | none |
| `worker->stop()` | (did not exist) | closes the MCAP file | none |
| Slow sink | its queue grew without bound | the channel's pool holds 64 snapshots by default; after that, snapshots are lost with `pool_exhausted` | none; `setPoolCapacity()` raises the bound |
| ROS 2 data topic QoS | reliable `KeepAll` | reliable `KeepLast(100)` | `options.data_qos = rclcpp::QoS(rclcpp::KeepAll()).reliable()` |
| ROS 2 schemas topic | reliable, transient-local `KeepAll`, published with the next snapshot | reliable, transient-local `KeepLast(1)`, published by `startLogging()`; each message holds every schema, as before | none |
| `LoggedValue::set()` | re-enabled a disabled value | stores only | call `setEnabled(true)` |
| Snapshot inside `scopedWrite()` on the same thread | (no `scopedWrite()`) | returns `blocked` | take the snapshot outside the transaction |
| MCAP message `sequence` | always 1 | 1, 2, 3, ... per channel and file | none |
| Negative snapshot timestamp | written, as a huge unsigned log time | not written by the MCAP sinks; counted in `errors()` | none |

Size the pool for the longest stall you want to absorb, before `startLogging()`:

```cpp
using namespace std::chrono_literals;
channel->setPoolCapacity(500ms, 1ms);  // 500 slots: a 500 ms disk stall at 1 kHz
```

`ChannelsRegistry::setChannelDefaults()` applies the same setting to every new channel.
Watch `channel->stats().pool_exhausted` to see whether the pool is large enough.

## Reading the data

The MCAP record layout and the ROS 2 messages `Schema`, `Schemas` and `Snapshot` are
unchanged. `SnapshotBatch` is new and used only when `ROS2PublisherOptions::aggregate`
is set. What changed:

- The schema text is version 5, and its hash is FNV-1a of the text: the same on every
  platform, and different from the 1.x hash of the same channel. Readers built with the
  1.x parser reject version 5 with "Wrong SCHEMA_VERSION", and the optional YAML
  rendering (`SchemaFormat::Yaml`, version 6) with "Unexpected line: fields:".
- A `std::string`, registered or inside a custom type, is written as a `uint32` count and
  its characters. 1.x wrote a 16-bit length that no reader decoded.

So:

- update PlotJuggler to a release that bundles the 2.0 parser;
- in your own C++ readers, copy the 2.0
  [data_tamer_v2/data_tamer_parser.hpp](../data_tamer_cpp/include/data_tamer_v2/data_tamer_parser.hpp);
- 2.0 readers still read files recorded with 1.x (schema version 4), using the hash the
  file declares. They reject schema texts without a `### version:` line, written by
  releases before 0.7.0 (late 2023), among them the header-less lines of the 0.3 to 0.5
  ROS 2 publisher, which the 1.x parser read.

The parser header changed too:

- `BuilSchemaFromText()` is now `BuildSchemaFromText()`. The old name remains for one
  release, deprecated.
- `ParseSnapshot()` lost its unused `callback_custom` parameter, and its result is
  `[[nodiscard]]`. It returns false for a hash mismatch, as before, and also when bytes
  are left after the last field. It throws `std::runtime_error` on malformed data.
- `ParseSnapshotRecursive` moved to `DataTamerParser::detail`, and `NullCustomCallback`
  is removed.
- `SnapshotView::schema_hash` is `uint64_t`.
- The header needs a little-endian host and no longer includes `<iostream>`.
- New: `SplitMcapMessage()` splits an MCAP message into mask and payload, and
  `SchemaRegistry`, `ToSnapshotView()` and `ForEachSnapshotInBatch()` decode the ROS 2
  messages.

Python has a decoder now, `pip install "data-tamer-parser[mcap]"` (see
[Reading the data](USER_MANUAL.md#reading-the-data)).

## Building and linking

- The target names are unchanged: `data_tamer_cpp::data_tamer` under ROS 2,
  `data_tamer::data_tamer` otherwise. Consumers still need C++17. Building the library
  needs CMake 3.22 and a C++20 compiler.
- The shared library's SONAME is `libdata_tamer.so.2`. In 1.x it was the bare
  `libdata_tamer.so`, which the 2.0 install still provides as a symlink, so a 1.x binary
  that is not rebuilt loads the 2.0 library and fails on missing symbols.
- Without ROS, `find_package(data_tamer 2 REQUIRED)` accepts any 2.x. The bundled MCAP
  headers are installed with the library, which also contains the MCAP implementation:
  don't define `MCAP_IMPLEMENTATION` in a program that links data_tamer.
- Under ROS 2, data_tamer links `mcap_vendor` privately: a program that uses MCAP itself
  links `mcap_vendor::mcap`. `ros2_publisher_sink.hpp` no longer includes the
  `data_tamer_msgs` headers: include the ones you use.
- From 2.0 on, a program built against any 2.x release runs with any later 2.x library.
  `ROS2PublisherSink` is the exception: its ABI also follows the rclcpp release.
