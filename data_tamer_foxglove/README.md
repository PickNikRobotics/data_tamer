# @data-tamer/parser

Synchronous TypeScript ESM decoder for data_tamer snapshots, version 2.0.0.
Node 22 or an ES2022 browser; no runtime dependencies. The Foxglove extension
is in [`extension/`](extension/README.md): it bundles these sources for ROS 2
topics and rosbag2 recordings, with its own build, tests and `.foxe` packaging.

```ts
import {
  parseSchema, schemaHash, splitMcapMessage, decodeFlat, decodeMessage,
  SchemaRegistry, decodeSnapshotMsg, decodeSnapshotBatch,
} from '@data-tamer/parser';

const schema = parseSchema(schemaText, { verifyHash: true });
const { activeMask, payload } = splitMcapMessage(mcapMessageBody);
const { names, values } = decodeFlat(schema, activeMask, payload);
const message = decodeMessage(schema, activeMask, payload);

const registry = new SchemaRegistry({ verifyHash: true });
registry.addSchemas(schemasMsg); // { schemas: [{ hash, schema_text }] }
const snapshot = decodeSnapshotMsg(registry, snapshotMsg);
// undefined if unknown; otherwise { schema, timestamp_nsec, values }
const snapshots = decodeSnapshotBatch(registry, batchMsg);
// Embedded schemas register first; unknown hashes skip; input order is retained.
```

`parseSchema` accepts v4/v5 text and v5 YAML and returns a frozen schema.
`schemaHash` computes FNV-1a-64 (YAML uses canonical v5 text). V4 hashes are
accepted unchecked because their `std::hash` recipe is platform dependent.
Registry hashes accept `bigint` or safe integer numbers; unsafe numbers fail
with `bad_hash`. `registry.add(hash, text)` and `registry.find(hash)` also work
directly. ROS byte fields accept `Uint8Array` or validated `number[]`.
Timestamps retain their supplied type; use `bigint` for exact nanoseconds.
MCAP framing returns views into its input buffer.

Numbers and names:

- Bool is numeric 0/1; char is its byte value. int64/uint64 become JS numbers,
  exact through 2^53. Hashes remain `bigint`.
- A `string` field (a C++ `std::string` or `std::string_view`) is one JS string:
  UTF-8, invalid bytes replaced by U+FFFD, a leading BOM kept, `""` when empty;
  also in `string[N]`, `string[]` and custom types. A `char[]` or `char[N]` is
  per character, one byte value each.
- Float32 widens to binary64. Every non-NaN bit pattern, including -0, is exact;
  NaNs are equivalent regardless of payload, sign or signaling bit.
- Flat results retain payload order. Duplicate names keep their first position
  and last value; disabled fields are absent. The same cached layout returns
  the identical frozen names array; compare contents after eviction.
- Nested objects split `/` into keys. Terminal numeric bracket suffixes of
  original names become array indices (`a[b][0]` → `message['a[b]'][0]`);
  actual container indices are appended directly. Empty slash keys remain.
  Bracket suffixes outside JS's valid array-index range remain literal text.
- A value also used as an object/array prefix, or incompatible object/array
  paths, throws `name_conflict`. Numeric aliases use the last payload value.
  `__proto__` and `constructor` remain safe own properties.

Limits: nesting of custom types is checked during decoding at depth 64;
finite recursive vectors are valid. Counts are checked against remaining bytes
before element allocation. Unknown/opaque enabled types fail, except empty
opaque vectors. Caches retain at most 16 masks/schema, 8 general layouts or
32 basic-vector name layouts/mask, and 65,536 expanded names/counts per mask.
Oversized layouts decode without retention. The caller owns the schema catalog.
Malformed input throws `DataTamerError` with `code`: `bad_schema`,
`missing_version`, `unsupported_version`, `hash_mismatch`, `bad_hash`,
`truncated`, `trailing_bytes`, `short_mask`, `opaque_type`, `unknown_type`,
`too_deep`, `bad_count` (invalid ROS bytes), or `name_conflict`. The extension
also uses `bad_timestamp` for invalid snapshot timestamps.

```sh
npm ci
npm run typecheck
npm test
npm run bench
DATATAMER_DIFF_GROUPS=5000 npm test
```

Tests need Python 3.9+ (`python3`), stdlib only. A fixed-seed generator runs with
`python3 -I`, imports the repository reference and shared random test helpers,
and writes an ephemeral oracle to ignored `dist/reference.json`. Tests compare
both flat and nested results, golden fixtures, the v4 recording via `@mcap/core`,
malformed inputs, recursive types and cache bounds. Unknown Python errors fail
the generator. The benchmark prints measured median/min/spread in ns/snapshot
for fixed, mixed, int64 and varying-vector shapes; parsing is outside timing.
