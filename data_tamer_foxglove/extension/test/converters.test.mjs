import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import {createConverterState, convertSchemas, convertSnapshot, convertSnapshotBatch, timestampToStamp,} from '../.test-dist/extension/src/converters.js';
import {activate} from '../.test-dist/extension/src/index.js';
import {DataTamerError} from '../.test-dist/src/index.js';

const vectors = new URL('../../../docs/wire_format/vectors/', import.meta.url);
const text = readFileSync(new URL('schema.txt', vectors), 'utf8');
const hash = 11691003255120904065n;
const schema = {
  hash,
  channel_name: 'ignored: use the schema text',
  schema_text: text
};
const snapshot = {
  schema_hash: hash,
  timestamp_nsec: 1700000000123456789n,
  active_mask: new Uint8Array(readFileSync(new URL('snapshot_full.mask', vectors))),
  payload: new Uint8Array(readFileSync(new URL('snapshot_full.payload', vectors))),
};
const event =
    (message, topic = '/robot/data', schemaName = 'data_tamer_msgs/msg/Snapshot') => ({
      topic,
      schemaName,
      message,
      receiveTime: { sec: 99, nsec: 0 },
      sizeInBytes: 0,
    });
function setup()
{
  const warnings = [];
  const state = createConverterState((...args) => warnings.push(args));
  return { state, warnings };
}
function catalog(state)
{
  return convertSchemas({ schemas: [schema] }, event(null, '/robot/schemas'), state);
}

test('Schemas registers a catalog used by snapshots on another prefix', () => {
  const { state, warnings } = setup();
  assert.deepEqual(
      catalog(state),
      { schemas: [{ channel: 'wire_test', hash: String(hash), fields: 16 }], errors: 0 });
  const out = convertSnapshot(snapshot, event(snapshot, '/other/data'), state);
  assert.equal(out.channel, 'wire_test');
  assert.deepEqual(
      out.header, { stamp: { sec: 1700000000, nsec: 123456789 }, frame_id: '' });
  assert.equal(out.values.points[0].x, 4);
  assert.equal(warnings.length, 0);
});

test('unknown schemas skip, then decode after the catalog arrives', () => {
  const { state, warnings } = setup();
  assert.equal(convertSnapshot(snapshot, event(snapshot), state), undefined);
  assert.equal(warnings.length, 0);
  catalog(state);
  assert.ok(convertSnapshot(snapshot, event(snapshot), state));
});

test('malformed snapshots warn once per topic and code without throwing', () => {
  const { state, warnings } = setup();
  catalog(state);
  const bad = {...snapshot, payload: [] };
  for(let i = 0; i < 3; i++)
    assert.equal(convertSnapshot(bad, event(bad), state), undefined);
  assert.equal(warnings.length, 1);
  assert.match(warnings[0][0], /\/robot\/data.*truncated/);
  assert.equal(convertSnapshot(bad, event(bad, '/other/data'), state), undefined);
  const badMask = {...snapshot, active_mask: [] };
  assert.equal(convertSnapshot(badMask, event(badMask), state), undefined);
  assert.equal(warnings.length, 3);
});

test(
    'invalid message shapes, hashes and schemas are contained at converter boundaries',
    () => {
      const { state, warnings } = setup();
      for(const value of [null, {}, { schemas: null }])
      {
        assert.equal(convertSchemas(value, event(value), state), undefined);
        assert.equal(convertSnapshot(value, event(value), state), undefined);
        assert.equal(convertSnapshotBatch(value, event(value), state), undefined);
      }
      assert.equal(
          convertSnapshot(
              {...snapshot, schema_hash: Number.MAX_VALUE }, event(null), state),
          undefined);
      const bad = { schemas: [{...schema, hash: hash + 1n }] };
      assert.deepEqual(
          convertSchemas(bad, event(bad), state), { schemas: [], errors: 1 });
      assert.ok(warnings.some(([message]) => message.includes('bad_hash')));
      assert.ok(warnings.some(([message]) => message.includes('hash_mismatch')));
    });

test(
    'batch embedded schemas decode without the catalog and retain valid siblings in order',
    () => {
      const { state, warnings } = setup();
      const batch = {
        schemas: [schema],
        snapshots: [
          snapshot,
          {...snapshot, payload: [] },
          {...snapshot, schema_hash: 7n },
          {...snapshot, timestamp_nsec: 2000000003n },
        ]
      };
      const out = convertSnapshotBatch(batch, event(batch, '/robot/data_batch'), state);
      assert.equal(out.snapshots.length, 2);
      assert.deepEqual(out.snapshots.map(s => s.header.stamp), [
        { sec: 1700000000, nsec: 123456789 },
        { sec: 2, nsec: 3 },
      ]);
      assert.equal(out.snapshots[0].values.points[0].x, 4);
      assert.equal(warnings.length, 1);
    });

test('batches without embedded schemas use the same registry as Schemas', () => {
  const { state } = setup();
  catalog(state);
  const batch = { schemas: [], snapshots: [snapshot] };
  assert.equal(
      convertSnapshotBatch(batch, event(batch), state).snapshots[0].channel, 'wire_test');
});

test('disabled fields remain absent from nested output', () => {
  const { state } = setup();
  catalog(state);
  const empty = {...snapshot, active_mask: [0, 0, 0], payload: [] };
  assert.deepEqual(convertSnapshot(empty, event(empty), state).values, {});
});

test(
    'timestamp conversion splits bigint and integer numbers, including rounded epoch numbers',
    () => {
      for(const [input, expected] of [
              [0n, { sec: 0, nsec: 0 }],
              [1000000001n, { sec: 1, nsec: 1 }],
              [2999999999, { sec: 2, nsec: 999999999 }],
              [1700000000000000000, { sec: 1700000000, nsec: 0 }],
              [18446744073709551615n, { sec: 18446744073, nsec: 709551615 }],
      ])
        assert.deepEqual(timestampToStamp(input), expected);
      for(const input of [-1n, -1, 1.5, NaN, Infinity, '1', 1n << 64n])
        assert.throws(
            () => timestampToStamp(input),
            error => error instanceof DataTamerError && error.code === 'bad_timestamp');
      const { state, warnings } = setup();
      catalog(state);
      const bad = {...snapshot, timestamp_nsec: -1 };
      assert.equal(convertSnapshot(bad, event(bad), state), undefined);
      assert.equal(warnings.length, 1);
      assert.match(warnings[0][0], /bad_timestamp/);
    });

test(
    'Schemas retains valid entries around failures and warns per topic, hash and code',
    () => {
      const { state, warnings } = setup();
      const bad = h => ({ hash: h, schema_text: '### version: 7\n' });
      const input = { schemas: [bad(1n), schema, bad(2n), bad(1n), null] };
      assert.deepEqual(convertSchemas(input, event(input, '/robot/schemas'), state), {
        schemas: [{ channel: 'wire_test', hash: String(hash), fields: 16 }],
        errors: 4,
      });
      assert.equal(
          convertSnapshot(snapshot, event(snapshot), state).values.points[0].x, 4);
      assert.equal(warnings.length, 3);
      assert.match(warnings[0][0], /schema 1.*unsupported_version/);
      assert.match(warnings[1][0], /schema 2.*unsupported_version/);
      convertSchemas(input, event(input, '/robot/schemas'), state);
      assert.equal(warnings.length, 3);
      convertSchemas({ schemas: [bad(1n)] }, event(null, '/other/schemas'), state);
      assert.equal(warnings.length, 4);
      convertSchemas(
          { schemas: [{ hash: 1n, schema_text: '' }] }, event(null, '/robot/schemas'),
          state);
      assert.equal(warnings.length, 5);
      assert.match(warnings[4][0], /schema 1.*bad_schema/);
    });

test(
    'rejected schemas bypass registry parsing across catalogs and embedded batches',
    () => {
      const { state, warnings } = setup();
      const add = state.registry.add.bind(state.registry);
      let attempts = 0;
      state.registry.add = (...args) => {
        attempts++;
        return add(...args);
      };
      const bad = { hash: 7n, schema_text: '### version: 7\n' };
      const input = { schemas: [bad] };
      const ev = event(input, '/robot/data_batch');
      assert.deepEqual(convertSchemas(input, ev, state), { schemas: [], errors: 1 });
      assert.deepEqual(convertSchemas(input, ev, state), { schemas: [], errors: 1 });
      assert.deepEqual(
          convertSchemas({ schemas: [{...bad, hash: 8n }] }, ev, state),
          { schemas: [], errors: 1 });
      assert.deepEqual(
          convertSnapshotBatch({...input, snapshots: [] }, ev, state), { snapshots: [] });
      assert.equal(attempts, 1);
      assert.equal(warnings.length, 2);
    });

test('negative cache evicts FIFO at 256 entries without refreshing hits', () => {
  const { state } = setup();
  const add = state.registry.add.bind(state.registry);
  let attempts = 0;
  state.registry.add = (...args) => {
    attempts++;
    return add(...args);
  };
  const bad = i => ({ hash: 7n, schema_text: `### version: 7\nuint8 x${i}\n` });
  const reject = i => convertSchemas({ schemas: [bad(i)] }, event(null), state);
  for(let i = 0; i < 256; i++)
    reject(i);
  assert.equal(attempts, 256);
  reject(0);
  assert.equal(attempts, 256);
  reject(256);
  assert.equal(attempts, 257);
  reject(1);
  assert.equal(attempts, 257);
  reject(0);
  assert.equal(attempts, 258);
  assert.equal(state.rejectedSchemas.size, 256);
});

test(
    'a negative cache entry does not poison valid text supplied later with its correct hash',
    () => {
      const { state } = setup();
      const bad = { schemas: [{...schema, hash: hash + 1n }] };
      assert.deepEqual(
          convertSchemas(bad, event(bad), state), { schemas: [], errors: 1 });
      assert.equal(catalog(state).errors, 0);
      assert.equal(
          convertSnapshot(snapshot, event(snapshot), state).channel, 'wire_test');
    });

test('a rejected embedded schema does not discard valid schemas or snapshots', () => {
  const { state, warnings } = setup();
  const batch = {
    schemas: [{ hash: 7n, schema_text: '### version: 7\n' }, schema],
    snapshots: [snapshot, {...snapshot, schema_hash: 7n }],
  };
  const out = convertSnapshotBatch(batch, event(batch), state);
  assert.equal(out.snapshots.length, 1);
  assert.equal(out.snapshots[0].values.points[0].x, 4);
  assert.equal(warnings.length, 1);
});

test('activation registers only the three stable schema conversions', () => {
  const registrations = [];
  activate({ registerMessageConverter: args => registrations.push(args) });
  assert.deepEqual(registrations.map(r => [r.type, r.fromSchemaName, r.toSchemaName]), [
    ['schema', 'data_tamer_msgs/msg/Schemas', 'data_tamer.Schemas'],
    ['schema', 'data_tamer_msgs/msg/Snapshot', 'data_tamer.Snapshot'],
    ['schema', 'data_tamer_msgs/msg/SnapshotBatch', 'data_tamer.SnapshotBatch'],
  ]);
});

test('a string field converts to a string, a char[] to byte values', () => {
  const { state, warnings } = setup();
  const strings_hash = 693923379855050783n;
  convertSchemas(
      {
        schemas: [{
          hash: strings_hash,
          schema_text: readFileSync(new URL('schema_strings.txt', vectors), 'utf8')
        }]
      },
      event(null, '/robot/schemas'), state);
  const message = {
    schema_hash: strings_hash,
    timestamp_nsec: 1n,
    active_mask: new Uint8Array(readFileSync(new URL('snapshot_strings.mask', vectors))),
    payload: new Uint8Array(readFileSync(new URL('snapshot_strings.payload', vectors))),
  };
  const values = convertSnapshot(message, event(message), state).values;
  assert.equal(values.ascii, 'data_tamer');
  assert.equal(values.empty, '');
  assert.equal(values.utf8, 'Zürich ✓');
  assert.deepEqual(values.labels, [{ id: 1, text: 'x' }, { id: 2, text: '' }]);
  assert.deepEqual(values.fixed, [97, 98, 99]);
  assert.deepEqual(values.chars, [104, 105]);
  assert.deepEqual(values.tags, ['t', '']);
  assert.equal(warnings.length, 0);
});
