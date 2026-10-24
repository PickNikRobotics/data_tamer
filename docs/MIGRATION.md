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
   Change the namespace `DataTamer` to `DataTamer2` (`DataTamer2::LogChannel`,
   `using namespace DataTamer2;`). `SerializeMe` and `DataTamerParser` keep their names.
3. Create sinks with their `create()` function, and port custom sinks to `DataSink`.
4. Replace `if(!channel->takeSnapshot())` with `if(!accepted(channel->takeSnapshot()))`.
5. In other threads, replace `writeMutex()` with `scopedWrite()`.
6. Replace `finishQueueAndStop()` and the `*AcceptingSnapshots()` calls with
   `worker->stop()` and `start()`, or `ChannelsRegistry::Global().stopAll()` (`shutdown()`
   at the end of `main()`, which also drops the channels that borrow your variables). A
   worker also stops when its last reference goes away (`~SinkWorker` calls `stop()`), as
   a 1.x sink finished its file in its destructor: call `stop()` where the recording must
   be complete at a known point.
7. Check the [behaviour changes](#behaviour-changes): MCAP files now roll over, the
   pool now drops snapshots, and the ROS 2 QoS changed.
8. Update every reader of the recorded data: 1.x parsers can't read 2.0 files.

## Quick reference

| 1.x | 2.0 |
|---|---|
| `#include "data_tamer/channel.hpp"` (every header) | `#include "data_tamer_v2/channel.hpp"` |
| `#include "data_tamer_parser/data_tamer_parser.hpp"` | `#include "data_tamer_v2/data_tamer_parser.hpp"` |
| `DataTamer::LogChannel`, `namespace DataTamer` | `DataTamer2::LogChannel`, `namespace DataTamer2` |
| `std::make_shared<MCAPSink>(path)` | `MCAPSink::create(path)`, a `std::shared_ptr<SinkWorkerOf<MCAPSink>>` that converts to `std::shared_ptr<SinkWorker>`; `addDataSink()`, `removeDataSink()` or `addDefaultSink()` of the bare sink fails to compile with a `static_assert` that says so |
| `std::make_shared<ROS2PublisherSink>(node, prefix)` | `ROS2PublisherSink::create(node, prefix)` |
| `std::make_shared<DummySink>()` | `DummySink::create()` |
| `sink->setMaxTimeBeforeReset(t)` and other sink methods | `worker->sink().setMaxTimeBeforeReset(t)`; `worker->as<MCAPSink>()` through a plain `SinkWorker` |
| `MCAPSink::finishQueueAndStop()` | `worker->stop()`, which also closes the file |
| `class MySink : public DataSinkBase` | `class MySink : public DataSink`, see [custom sinks](#custom-sinks) |
| `if(!channel->takeSnapshot())` | `if(!accepted(channel->takeSnapshot()))` (`ok` or `partial`) |
| `channel->getActiveFlags()` | removed: `channel->isEnabled(id)` or `value->isEnabled()` per value; a sink reads `Snapshot::active_mask` |
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
| `DataTamer::AddFieldToHash()` (`types.hpp`) | `SchemaTextHash()`, `ComputeSchemaHash()`; the parser's `DataTamerParser::AddFieldToHash()` remains, for version 4 texts |
| `channel->addDataSink(sink)` returned `void` | it returns `bool`: `false` if the sink was attached already |
| `SerializeMe::enable_if_same_t` | removed: `std::enable_if_t<std::is_same_v<A, B>>` |
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
auto worker = DataTamer2::MCAPSink::create("run.mcap");
worker->sink().setMaxTimeBeforeReset(std::chrono::seconds(60));
DataTamer2::ChannelsRegistry::Global().addDefaultSink(worker);
```

`worker->sink()` is the `MCAPSink`: `create()` returns a `SinkWorkerOf<MCAPSink>`. Code
that holds only a `std::shared_ptr<SinkWorker>` uses `worker->as<MCAPSink>()`, which throws
`std::bad_cast` if the sink is not an `MCAPSink`. `addDefaultSink()` also
attaches the sink to channels that exist already, so it no longer has to come before
`getChannel()`. `ROS2PublisherSink::create()` takes an optional `ROS2PublisherOptions`
third argument. A channel holds at most 8 sinks, and so does the registry as default
sinks: a ninth throws `std::runtime_error`.

To stop a sink, call `worker->stop()`, or `ChannelsRegistry::Global().stopAll()` for every
sink of the registry. At the end of `main()`, `ChannelsRegistry::Global().shutdown()` does
`stopAll()` and then drops the channels. `stop()` delivers what is queued, then finishes
the sink: `MCAPSink` closes its file. `worker->start()` resumes, and `MCAPSink` continues
in its next numbered file. `MCAPSink::restartRecording()` still opens a new file, but it no longer resumes a
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

class CsvSink : public DataTamer2::DataSink
{
protected:
  void onSchema(const DataTamer2::Schema& schema) override
  {
    names_[schema.hash] = schema.channel_name;
  }
  void onSnapshot(const DataTamer2::SnapshotRef& snapshot) override
  {
    if(!write(names_[snapshot->schema_hash], snapshot->payload))
    {
      throw std::runtime_error("CsvSink: write failed");  // counted in errors()
    }
  }

private:
  std::unordered_map<uint64_t, std::string> names_;
  bool write(const std::string& channel, const DataTamer2::PayloadVector& payload);
};
auto worker = DataTamer2::SinkWorker::create<CsvSink>();
```

| 1.x | 2.0 |
|---|---|
| `addChannel(name, schema)`, public | `onSchema(schema)`, protected; the name is `schema.channel_name` |
| `bool storeSnapshot(const Snapshot&)` | `void onSnapshot(const SnapshotRef&)`; throw to report a failure |
| return `false` from `storeSnapshot()` | throw: `worker->errors()` and `lastError()` count and keep it |
| override `pushSnapshot()` | removed: the channel pushes into the worker's queue |
| call `addChannel()` and `pushSnapshot()` on a sink yourself | removed: only a channel feeds a worker; see [writing MCAP files yourself](#writing-mcap-files-yourself) |
| `stopThread()` in the destructor | delete it: the worker stops before it destroys the sink |
| `stopAcceptingSnapshots()`, `processQueuedSnapshots()`, `startAcceptingSnapshots()` | `worker->stop()`, `worker->drain()`, `worker->start()`; `stop()` also calls the sink's `onStop()` and `start()` its `onStart()`, so a sink that closes its file there splits the recording (`MCAPSink` continues in its next numbered file) |
| close files in the destructor | override `onStop()`, and `onStart()` to reopen them |
| `Snapshot::schema_hash` was `size_t` | it is `uint64_t` |

A `SnapshotRef` holds a slot of the channel's snapshot pool, which all sinks of the
channel share. Return from `onSnapshot()` quickly. For slow work, copy the snapshot
(`DataTamer2::Snapshot copy = *snapshot;`) and hand the copy to a thread of your own.
Keep functions that change the channel (registration, sinks, `startLogging()`) out of
callbacks: a control call that would wait for the callback's own worker throws
`std::logic_error` there instead of deadlocking (the list is in the `DataSink`
documentation, `data_sink.hpp`).

`DummySink` has accessors instead of public members: `latestSnapshot()`,
`latestPayloadSize()`, `latestActiveMask()`, `snapshotsCount(hash)`, `schemasCount()`,
`firstSchemaHash()` and `schema(hash)`. `schema_names` is gone: use
`schema(hash).channel_name`. In tests, call `worker->drain()` before reading them,
instead of sleeping.

### Writing MCAP files yourself

In 1.x, `addChannel()` and `pushSnapshot()` were public, so code could feed an `MCAPSink`
by hand, for instance to write stored snapshots. In 2.0 the sink callbacks are protected
and only a channel feeds a worker. To write a data_tamer MCAP file from your own data, use
the encoding that `MCAPSink` uses, `data_tamer_v2/sinks/mcap_encoding.hpp`:

```cpp
#include "data_tamer_v2/sinks/mcap_encoding.hpp"
namespace enc = DataTamer2::mcap_encoding;

mcap::McapWriter writer;
if(!writer.open("stored.mcap", mcap::McapWriterOptions(enc::kEncoding)).ok())
{
  // handle the error
}
const auto id = enc::AddChannel(writer, schema);  // once per schema and file
std::vector<uint8_t> scratch;                     // reused across messages
uint32_t sequence = 1;
for(const DataTamer2::Snapshot& snapshot : stored)
{
  if(!enc::WriteSnapshot(writer, id, sequence++, snapshot, scratch).ok())
  {
    // handle the error
  }
}
writer.close();
```

`WriteMessage()` takes a timestamp, a mask and a payload instead of a `Snapshot`. Link
`data_tamer::data_tamer`, which provides the MCAP headers, and don't define
`MCAP_IMPLEMENTATION` (see [Building and linking](#building-and-linking)).

## Taking snapshots

`takeSnapshot()` returns a `SnapshotResult` instead of a `bool`. `ok` means every sink
queued the snapshot (delivery comes later, on the worker thread:
`SinkWorker::delivered()` counts it); the
[user manual](USER_MANUAL.md#snapshots-are-missing-from-the-recording) lists the other
values. A plain `channel->takeSnapshot();` still compiles, but
`if(!channel->takeSnapshot())` doesn't. `accepted()` is true when a sink took it (`ok` or
`partial`):

```cpp
if(!DataTamer2::accepted(channel->takeSnapshot()))
{
  // count it, or read the totals from channel->stats()
}
```

`channel->stats()` counts the calls (`attempts`) and the snapshots a sink took
(`accepted`: `ok` and `partial`). `pool_exhausted`, `oversize` and `serialize_failed` have
counters of their own (`pool_exhausted`, `dropped_oversize`, `serialize_failures`), and
`blocked` is in `write_lock_contended`, with the `takeSnapshot()` calls that waited.
`rejected` and `no_sinks` have none (nor `start_failed`, which only `tryTakeSnapshot()`
returns): `attempts - accepted` covers them with the others.

`getActiveFlags()` is removed. A sink reads the mask of each snapshot from
`Snapshot::active_mask`; the application asks `channel->isEnabled(id)`, or
`value->isEnabled()` for a `LoggedValue`.

2.0 adds `startLogging()`, which freezes the schema, allocates the snapshot pool and sends
the schema to the sinks. The first `takeSnapshot()` calls it for you, so 1.x code needs no
change. Call it yourself before the loop when the first iteration must be fast, or with
`ROS2PublisherSink`, which publishes the schemas from `startLogging()`. A real-time loop
calls the new `tryTakeSnapshot()`, which never blocks or allocates, except in its first
call with a sink, which starts logging: call `startLogging()` before the loop when the first
cycle must be real-time (see [Real-time use](USER_MANUAL.md#real-time-use)).

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

`LoggedValue` changed in five ways:

- Scalars (numbers, `bool`, `char`, enums) are `std::atomic`. Use `set()` and `get()`:
  `getMutablePtr()` and `getConstPtr()` on a scalar fail with "scalar LoggedValues are
  atomic: use set()/get()".
- For structs and vectors, `getMutablePtr()` and `getConstPtr()` remain. The guards are not
  movable and have no `operator bool`. `getLockedPtr()` is removed.
- For structs and vectors, `set()` and `get()` lock the channel's write mutex, as
  `scopedWrite()` does, so they wait while a snapshot serializes. In 1.x they locked only
  a mutex of the value, which the snapshot did not take. Don't call them on a real-time
  thread that must not block.
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
  contain control characters. `DataTamer2::JoinNames()` (`data_tamer_v2/names.hpp`) builds
  `a/b/c` names without empty components.
- `long long`, `unsigned long long`, `wchar_t`, `char16_t` and `char32_t` are recorded as
  integers of their size. 1.x recorded them as `other`, which no reader decodes.
  `long double` is a compile error ("has no wire type").
- Two C++ types with the same custom type name in one channel make `registerValue()`
  throw. In 1.x the second one was serialized with the first one's serializer.
- `CustomTypeName<T>::get()` is no longer `constexpr`, and `Schema::custom_types` and
  `custom_schemas` are `std::map` (they were `std::unordered_map`).
- New: `registerValue()` accepts a `const std::atomic<T>*`, and
  `DataTamer2::TypeDefinitionTrait<T>` describes a type whose namespace you can't reopen
  (see [custom types](USER_MANUAL.md#custom-types)). Existing `TypeDefinition()`
  functions keep working.
- `getPointerType()` is removed.

As in 1.x, registering a new name after `startLogging()` or the first snapshot with a sink
throws.

## Behaviour changes

These changes compile without a warning.

| Area | 1.x | 2.0 | To get the 1.x behaviour |
|---|---|---|---|
| MCAP reset every 600 s | truncated the file, losing everything recorded before | continues in a new numbered file; disk use is unbounded | `setCreateNewFileOnReset(false)` |
| Rollover file names | `run.mcap_1` | `run_1.mcap`; numbered names that exist already are skipped, not overwritten (the path given to `create()` is overwritten, as in 1.x) | none |
| `worker->stop()`, `start()` | (did not exist) | `stop()` closes the MCAP file, `start()` continues in the next numbered file | none |
| Slow sink | its queue grew without bound | the channel's pool holds by default 8 MiB of snapshots, from 64 to 1024 (1 s at 1 kHz for a payload under 8 KB); after that, snapshots are lost with `pool_exhausted` | none; `setPoolCapacity()` raises the bound |
| ROS 2 data topic QoS | reliable `KeepAll` | reliable `KeepLast(100)` | `options.data_qos = rclcpp::QoS(rclcpp::KeepAll()).reliable()` |
| ROS 2 schemas topic | reliable, transient-local `KeepAll`, published with the next snapshot | reliable, transient-local `KeepLast(1)`, published by `startLogging()`; each message holds every schema, as before | none |
| `LoggedValue::set()` | re-enabled a disabled value | stores only | call `setEnabled(true)` |
| `set()`, `get()` of a struct or vector `LoggedValue` | locked a mutex of the value only | lock the channel's write mutex: they wait for a snapshot in progress | none |
| An id kept across `unregister()` and a new registration of its name | acted on the new registration | stale: `setEnabled()` and `unregister()` throw `std::invalid_argument`, `trySetEnabled()` and `isEnabled()` return false | use the id the new registration returns |
| Snapshot on a thread that holds the write mutex (`scopedWrite()`, a `LoggedValue` guard) | deadlocked, with `writeMutex()` locked or a guard alive | returns `blocked` | take the snapshot outside the transaction |
| MCAP message `sequence` | always 1 | 1, 2, 3, ... per channel and file | none |
| Negative snapshot timestamp | written, as a huge unsigned log time | not written by the MCAP sinks; counted in `errors()` | none |

The default pool absorbs a disk flush. For a sink that stalls longer, size the pool for
the longest stall you want to absorb, before `startLogging()`:

```cpp
using namespace std::chrono_literals;
channel->setPoolCapacity(2s, 1ms);  // 2000 slots: a 2 s stall at 1 kHz
```

`ChannelsRegistry::setChannelDefaults()` applies the same setting to every new channel.
Watch `channel->stats().pool_exhausted` to see whether the pool is large enough.

The pool belongs to one channel and is allocated by `startLogging()`, so memory grows with
the number of channels: about 8 MiB each by default, less for a small payload (1024 slots
of about 128 bytes plus the payload reservation) and more above 128 KB (64 slots). Ten
channels with payloads of 8 KB reserve about 80 MiB. Lower the count with
`setPoolCapacity()` or `setChannelDefaults()` where memory is tight.

## Reading the data

The MCAP record layout and the ROS 2 messages `Schema`, `Schemas` and `Snapshot` are
unchanged. `SnapshotBatch` is new and used only when `ROS2PublisherOptions::aggregate`
is set. What changed:

- The schema text is version 5, and its hash is FNV-1a of the text: the same on every
  platform, and different from the 1.x hash of the same channel. Readers built with the
  1.x parser reject version 5 with "Wrong SCHEMA_VERSION", and the optional YAML
  rendering (`SchemaFormat::Yaml`) with "Unexpected line: fields:".
- A `std::string` or `std::string_view`, registered, inside a custom type or in a
  container, is the new basic schema type `string`: a `uint32` byte count and its bytes.
  The decoders return it as one text value. 1.x wrote a `std::string` as `char[]` with a
  16-bit length that no reader decoded. A `std::vector<char>` is still `char[]`, one value
  per character.
- A custom type named after a basic type (`string`, `float64`, ...) is refused: the
  registration throws `std::runtime_error`. Rename the type.

So:

- update PlotJuggler to 4 or later, which bundles the 2.0 parser (3.x has the 1.x parser
  and rejects 2.0 files);
- in your own C++ readers, copy the 2.0
  [data_tamer_v2/data_tamer_parser.hpp](../data_tamer_cpp/include/data_tamer_v2/data_tamer_parser.hpp),
  or, with the library linked, read whole files with `ForEachValue()` of
  `data_tamer_v2/mcap_reader.hpp`;
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
- `ParseSnapshot(schema, view, callback_number)` skips `string` values. New:
  `ParseSnapshot(schema, view, callback_number, callback_string)` passes each one to
  `callback_string(name, std::string_view)`, and `ForEachValue()` takes the same extra
  callback. `BasicType` has a new last enumerator, `STRING`: a `switch` over it that lists
  every enumerator needs a case.
- New: `SplitMcapMessage()` splits an MCAP message into mask and payload, and
  `SchemaRegistry`, `DataTamerParser::ToSnapshotView()` of a ROS 2 message and
  `ForEachSnapshotInBatch()` decode the ROS 2 messages.

Python has a decoder now, `pip install "data-tamer-parser[mcap]"` (see
[Reading the data](USER_MANUAL.md#reading-the-data)).

## Building and linking

- Under ROS 2, 2.x is a new package, `data_tamer_v2`, released next to `data_tamer_cpp`
  1.x: replace `<depend>data_tamer_cpp</depend>` with `<depend>data_tamer_v2</depend>`,
  `find_package(data_tamer_cpp)` with `find_package(data_tamer_v2)` and the target
  `data_tamer_cpp::data_tamer` with `data_tamer_v2::data_tamer` (or use
  `find_package(data_tamer 2)` and `data_tamer::data_tamer`, which a ROS 2 install now
  provides too). The two install side by side in one prefix: the 2.x headers are in
  `include/data_tamer_v2` (see step 2), the library is `libdata_tamer_v2.so.2`, and the
  example runs as `ros2 run data_tamer_v2 ros2_publisher`. `data_tamer_msgs` keeps its
  name and messages. Port one package at a time: one process can load both, e.g. two
  plugins built against different majors, because they share no symbol. The library's
  namespace is `DataTamer2`, and `SerializeMe` and `DataTamerParser` sit in an inline
  namespace `v2`, which your code does not name.
- Without ROS the package is `data_tamer` and the target `data_tamer::data_tamer`, as in
  1.x. Consumers still need C++17. Building the library needs CMake 3.22 and a C++20
  compiler.
- Warnings no longer stop the build of the library: `-Werror` is the option
  `DATA_TAMER_WERROR`, OFF by default.
- As a subproject (`add_subdirectory()`, FetchContent) data_tamer installs nothing and
  builds no ROS 2 sink, tests or examples by default (`DATA_TAMER_INSTALL`,
  `DATA_TAMER_BUILD_ROS`, ...). Under ROS 2, `-DBUILD_TESTING=OFF` builds no test.
- Without ROS, the shared library's SONAME is `libdata_tamer.so.2`. In 1.x it was the bare
  `libdata_tamer.so`, which the 2.0 install still provides as a symlink, so a 1.x binary
  that is not rebuilt loads the 2.0 library and fails on missing symbols.
- `find_package(data_tamer 2 REQUIRED)` accepts any 2.x. Outside ROS 2, MCAP is 2.1.3
  (1.x bundled 1.3.0) and no longer in the repository: CMake downloads it, so the first
  configure needs network access (or `FETCHCONTENT_SOURCE_DIR_DATA_TAMER_MCAP`). Its
  headers are installed with the library, which also contains the MCAP implementation:
  don't define `MCAP_IMPLEMENTATION` in a program that links data_tamer. An MCAP package
  installed on the build machine is no longer used instead unless the build sets
  `DATA_TAMER_USE_SYSTEM_MCAP=ON`. The
  `mcap_lib` target is gone: link `data_tamer::data_tamer`.
- Under ROS 2, data_tamer links `mcap_vendor` publicly: the target gives a program the
  MCAP headers and `libmcap`. `ros2_publisher_sink.hpp` no longer includes the
  `data_tamer_msgs` headers: include the ones you use.
- From 2.0 on, a program built against any 2.x release runs with any later 2.x library.
  `ROS2PublisherSink` is the exception: its ABI also follows the rclcpp release.
