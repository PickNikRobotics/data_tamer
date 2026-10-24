# data_tamer for Foxglove

Plot data_tamer ROS 2 snapshots in Foxglove desktop or web, or inspect rosbag2
recordings of those topics (including rosbag2 MCAP storage). Version 2.0.0
supports schema versions 4 and 5 (line format and YAML).

## Install

Download `data_tamer.data-tamer-foxglove-2.0.0.foxe` from the GitHub release or
its pull-request workflow artifact. Install the local file in Foxglove's
Extensions settings, or drag it onto the Extensions sidebar. Reload the layout
if the conversions do not appear; UI labels may differ between app versions.

## Topics and schemas

For a `ROS2PublisherSink` prefix `/robot`:

| Topic | Source schema | Converted schema | Output |
| --- | --- | --- | --- |
| `/robot/schemas` | `data_tamer_msgs/msg/Schemas` | `data_tamer.Schemas` | `{ schemas: [{ channel, hash, fields }], errors }` |
| `/robot/data` | `data_tamer_msgs/msg/Snapshot` | `data_tamer.Snapshot` | `{ header, channel, values }` |
| `/robot/data_batch` | `data_tamer_msgs/msg/SnapshotBatch` | `data_tamer.SnapshotBatch` | `{ snapshots: [{ header, channel, values }, ...] }` |

Select the converted schema in the panel. A `std::string` value (schema type
`string`) is a string field (`.values.label`); a `std::vector<char>` or
`std::array<char, N>` is an array of byte values. Catalog hashes are decimal strings;
`fields` counts top-level fields before expanding arrays/custom types. `errors`
counts rejected entries in that catalog message; valid entries still register
and appear. The channel comes from the schema text.

**Open a Raw Messages panel subscribed to `<prefix>/schemas`, converted to
`data_tamer.Schemas`, at least once to load the catalog.** Registration alone
does not subscribe to it. The publisher's reliable, transient-local `KeepLast(1)`
catalog must reach the live bridge/subscriber, or be included in a recording.
Subscribe again for new catalogs. Unknown-schema snapshots are skipped, without
buffering: seek/replay after loading the catalog to revisit them.

All converters share one catalog until the extension reloads. v5 hashes
identify content. Legacy v4 hashes exclude custom type bodies: conflicting v4
catalogs with one hash are unsupported and could silently use the first schema.
Reload the extension between v4 recordings whose custom definitions changed.

Invalid snapshots are skipped. Console warnings identify errors once per
`(topic, code)` for snapshots, or `(topic, hash, code)` for rejected schemas.
Batches register embedded schemas first (included by default), then return
valid snapshots in input order; a rejected schema or snapshot does not discard
valid siblings. Without embedded schemas, load the catalog as above.

## Plot

Publish one `Snapshot` per message for a time series: this is the sink's default,
`aggregate = false`. Select `data_tamer.Snapshot` on `/robot/data`, then plot
`.values.positions[0]`, for example:

```text
/robot/data{channel=="controller"}.values.positions[0]
```

The `channel` filter selects one channel when several share the topic.
Choose header timestamps for Plot's x axis: every snapshot has
`header: { stamp: { sec, nsec }, frame_id: "" }`, from its `timestamp_nsec`,
independent of the receive time.

`SnapshotBatch` conversion is for Raw Messages inspection: look under
`.snapshots[0].values`. It returns one message per input batch, so it does not
expand snapshots into a time series. Batch expansion may come in a later release;
use `aggregate = false` to plot now.

## Numbers and series names

- `/` creates nested keys: `arm/position` becomes `.values.arm.position`.
  Terminal numeric brackets create array indices: `positions[0]` becomes
  `.values.positions[0]`. Actual container indices append directly; nonnumeric
  bracket text and empty slash components remain keys. Use quoted-key syntax
  for unusual names.
- Disabled series are absent, not zero. Duplicate names use the last payload
  value; conflicting value/object/array paths skip the snapshot (`name_conflict`).
- Values are JS numbers: bool is 0/1 and char is its byte value. Signed/unsigned
  64-bit integers are exact up to 2^53; larger values can round. Hashes require
  bigint or exact safe-integer numbers; unsafe numeric hashes are rejected.
  Bigint timestamps preserve nanoseconds; numbers already rounded by a reader
  cannot regain that precision. Invalid timestamps report `bad_timestamp`.

Files written directly by `MCAPSink` or `MCAPRingSink` use data_tamer's custom
encoding and are **not readable in Foxglove**. Use PlotJuggler 4 or the Python
decoder for those files.
