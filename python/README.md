# data-tamer-parser

Reference decoder for the [Data Tamer](https://github.com/facontidavide/data_tamer)
wire format, specified in
[docs/wire_format.md](https://github.com/facontidavide/data_tamer/blob/main/docs/wire_format.md).
A single module, `data_tamer_parser`, using the Python standard library only.

```
pip install data-tamer-parser           # the decoder
pip install "data-tamer-parser[mcap]"   # plus the mcap package, for iter_mcap()
pip install ./python                    # from a checkout of the repository
```

## MCAP files written by MCAPSink

```python
import data_tamer_parser as dt

for timestamp_nsec, topic, values in dt.iter_mcap("log.mcap"):
    print(topic, timestamp_nsec, values)   # values: {"pose/position/x": 1.0, "vec[2]": 3.0, ...}
```

Disabled fields are absent from `values`. Channels with another encoding are skipped.
A `string` (a `std::string` in the writer) is one `str` (`"label": "idle"`), its bytes
decoded as UTF-8 with invalid ones replaced by U+FFFD.
`iter_mcap()` reads one file: `MCAPSink` can split a recording into numbered files
(`run.mcap`, `run_1.mcap`, ...), each self-contained, so read them in turn.

Files of data_tamer 1.x (schema version 4) and 2.x are read; texts of releases before
0.7.0 raise `ValueError`. `verify_hash=True` checks the schema hash of a 2.x file, but
accepts the hash of a 1.x file unchecked: it came from the writer's `std::hash`.

## Columns for analysis

`parse_snapshot_values()` decodes a snapshot into two tuples, the names and the values in
payload order: `tuple(d)` and `tuple(d.values())` of `d = parse_snapshot(...)`, without
building the dict. Consecutive snapshots of one layout (the active mask and the length
of every dynamic vector) return the same `names` object. With pandas, one table per
layout:

```python
import pandas as pd
from mcap.reader import make_reader
import data_tamer_parser as dt

schemas, tables = {}, {}   # tables: names -> (log times, rows)
with open("log.mcap", "rb") as stream:
    for mcap_schema, channel, message in make_reader(stream).iter_messages(topics=["control"]):
        schema = schemas.get(mcap_schema.id)
        if schema is None:   # parse each schema once: it keeps its decoding plans
            schema = schemas[mcap_schema.id] = dt.parse_schema(mcap_schema.data.decode())
        mask, payload = dt.split_mcap_message(message.data)
        names, values = dt.parse_snapshot_values(schema, mask, payload)
        times, rows = tables.setdefault(names, ([], []))
        times.append(message.log_time)
        rows.append(values)

frames = [pd.DataFrame(rows, columns=list(names), index=pd.to_datetime(times, unit="ns"))
          for names, (times, rows) in tables.items()]
```

Only the last layout of each mask is kept, of up to 65536 names, so a layout can come
back as another tuple object, equal to the first: compare names with `==` (as the dict
above does), and use `is` only as a shortcut. If two fields flatten to the same name (a field `a/b` next to a field `a` with
a member `b`), `names` lists it once with its last value, as the dict does.

## Speed

`parse_snapshot()` compiles each schema and active mask once into `struct` formats and
keeps them on the `Schema`, so parse a schema once and reuse it. A run of fixed-size
values becomes one `unpack_from()` call, and so does a dynamic vector of a basic type or
of a custom type of fixed size. A vector whose elements hold vectors or strings is
decoded by the reference walker, at its speed. Microseconds per snapshot on Python 3.14
(`python3 python/benchmark_decode.py`):

| schema                           | values | reference | `parse_snapshot()` | `parse_snapshot_values()` |
|----------------------------------|-------:|----------:|-------------------:|--------------------------:|
| `float64[2]` x 3                 |      6 |       3.5 |         0.50 (7x)  |               0.20 (18x)  |
| 1000 `float64`                   |   1000 |       424 |           30 (14x) |                8.5 (50x)  |
| `float64[]`, 1000 long           |   1000 |       287 |           30 (9x)  |                9.3 (31x)  |
| `float64[]`, 900 to 1100 long    |  ~1000 |       287 |           30 (9x)  |                 16 (18x)  |
| `Pose[100]`                      |    700 |       330 |           21 (16x) |                6.1 (54x)  |
| `Point3D[]`, 100 long            |    300 |       126 |          9.0 (14x) |                3.3 (38x)  |
| `Joint[]`, a `char[]`, a float  |    156 |        63 |          68 (0.9x) |                 66 (1x)   |

`parse_snapshot_values()` keeps only the last layout of each mask, so when a vector
changes length at every snapshot, its names are copied again each time (the fourth row).

The schema walker that defines the result, `_parse_snapshot_reference()`, stays in the
module: the tests check that both functions agree with it, values and errors, on random
schemas, the golden vectors and the 1.0.4 recording. A `Schema` may be shared by
threads; do not modify one after decoding with it.

The caches are bounded. A `Schema` compiles at most 16 active masks, and the reference
walker decodes the snapshots of any further mask, at its speed; masks that differ only
in the bits past the last field share a plan. A plan compiles at most 65536 values (the
fields after them are walked) and keeps up to 65536 names of dynamic vectors and one
layout of up to 65536 names. That is a few MB for a schema of a thousand values, and
about 220 MB at worst: 16 masks of a schema of 65000 values and a 65000 long vector.

## Field names from a schema

`Schema.field_names()` lists the flattened names a schema can produce, without
decoding a message, so fields disabled in a given message are listed too:

```python
schema = dt.parse_schema(schema_text)
schema.field_names()   # ["pose/position/x", "arr[0]", "arr[1]", "vec[]", "points[]/x", ...]
```

Nested fields join with `/` and fixed arrays (`T[N]`) are expanded to `[0]`..`[N-1]`.
A dynamic vector (`T[]`) has a length known only per message, so its elements are
listed once with empty brackets (`vec[]`, `points[]/x`), where the decoder produces
`vec[0]`, `vec[1]`, ... The `[]` is a placeholder: a field whose own name ends in `[]`
would look the same. A field of an opaque custom type is listed under its own name, but
`parse_snapshot()` cannot decode a snapshot where it is enabled, unless it is a dynamic
vector with no element. `ValueError` is raised
for undefined or cyclic types, nesting deeper than the decoder accepts
(`MAX_SCHEMA_DEPTH`) and schemas that expand to more than `MAX_FIELD_NAMES`
(1,000,000) names.

## Versions and releases

The package is released together with the C++ library: a release tag, the
`package.xml` versions and `data_tamer_parser.__version__` are the same `X.Y.Z`. The
`python` workflow publishes to PyPI only for tags of that form, and fails if the tag
differs from `__version__`.

## Lower level

```python
schema = dt.parse_schema(schema_text)                  # line format or YAML
mask, payload = dt.split_mcap_message(mcap_message_body)
values = dt.parse_snapshot(schema, mask, payload)
```

Malformed input raises `ValueError`: a schema text, a message body that does not match
its two lengths, a mask shorter than the schema, a truncated payload or one with bytes
left over.

ROS 2 messages of `ROS2PublisherSink` (`data_tamer_msgs` `Schemas`, `Snapshot`,
`SnapshotBatch`) are decoded with `SchemaRegistry`, `parse_snapshot_msg()` and
`iter_snapshot_batch()`; they only read message attributes and need no ROS import.

```python
registry = dt.SchemaRegistry()     # registry.add_schemas(schemas_msg) for <prefix>/schemas
for schema, timestamp_nsec, values in dt.iter_snapshot_batch(registry, batch_msg):
    ...
```
