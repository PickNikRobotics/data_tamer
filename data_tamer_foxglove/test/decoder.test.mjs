import {bits, schema, separator as sep} from './helpers.mjs';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import {parseSchema, schemaHash, splitMcapMessage, decodeFlat, decodeMessage, SchemaRegistry, decodeSnapshotMsg, decodeSnapshotBatch} from '../dist/index.js';
const vectors = new URL('../../docs/wire_format/vectors/', import.meta.url);
const text = name => readFileSync(new URL(name, vectors), 'utf8');
const bytes = name => readFileSync(new URL(name, vectors));

const error = (code, fn) => assert.throws(fn, { name: 'DataTamerError', code });

for(const format of ['txt', 'yaml'])
  for(const mode of ['full', 'masked'])
  {
    test(`golden ${format} ${mode}`, () => {
      const s = parseSchema(text(`schema.${format}`), { verifyHash: true });
      assert.equal(s.hash, schemaHash(text('schema.txt')));
      const expected = JSON.parse(text('expected.json'))[mode];
      const mask = bytes(`snapshot_${mode}.mask`),
            payload = bytes(`snapshot_${mode}.payload`);
      const out = decodeFlat(s, mask, payload);
      assert.deepEqual(out.names, Object.keys(expected));
      assert.deepEqual(
          out.values.map(bits),
          Object.values(expected).map(
              v => bits(typeof v === 'string' ? v.charCodeAt(0) : +v)));
      assert.equal(decodeFlat(s, mask, payload).names, out.names);
      const body = splitMcapMessage(bytes(`snapshot_${mode}.mcap_message`));
      assert.deepEqual(body.activeMask, mask);
      assert.deepEqual(body.payload, payload);
      assert.equal(decodeMessage(s, mask, payload).points[0].x, 4);
    });
  }
test(
    'golden strings: a string is one JS string, a char[N] and a char[] stay per character',
    () => {
      const s = parseSchema(text('schema_strings.txt'), { verifyHash: true });
      const expected = JSON.parse(text('expected.json')).strings;
      const mask = bytes('snapshot_strings.mask'),
            payload = bytes('snapshot_strings.payload');
      const out = decodeFlat(s, mask, payload);
      assert.deepEqual(out.names, Object.keys(expected));
      assert.deepEqual(
          out.values,
          Object.entries(expected).map(
              ([name, v]) => /^(fixed|chars)\[/.test(name) ? v.charCodeAt(0) : v));
      const message = decodeMessage(s, mask, payload);
      assert.equal(message.utf8, 'Zürich ✓');
      assert.equal(message.invalid, 'a�b');
      assert.deepEqual(message.labels, [{ id: 1, text: 'x' }, { id: 2, text: '' }]);
      assert.deepEqual(message.fixed, [97, 98, 99]);
      assert.deepEqual(message.chars, [104, 105]);
      assert.deepEqual(message.tags, ['t', '']);
    });
const u32 = n => [n & 255, (n >> 8) & 255, (n >> 16) & 255, n >>> 24];
const str = b => [...u32(b.length), ...b];
const ascii = t => [...new TextEncoder().encode(t)];
test('a string at top level, in arrays and in a custom type', () => {
  const s =
      schema(`string a\nstring[2] pair\nstring[] list\nTagged t\nTagged[] ts\nchar[] c\n${
          sep}\nMSG: Tagged\nuint8 v\nstring label`);
  const payload = Uint8Array.from([
    ...str(ascii('top')),
    ...str(ascii('p0')),
    ...str([]),
    ...u32(2),
    ...str(ascii('l0')),
    ...str(ascii('long one')),
    7,
    ...str(ascii('tag')),
    ...u32(1),
    8,
    ...str([]),
    ...u32(2),
    0x68,
    0x69,
  ]);
  const out = decodeFlat(s, Uint8Array.of(63), payload);
  assert.deepEqual(out.names, [
    'a', 'pair[0]', 'pair[1]', 'list[0]', 'list[1]', 't/v', 't/label', 'ts[0]/v',
    'ts[0]/label', 'c[0]', 'c[1]'
  ]);
  assert.deepEqual(
      out.values, ['top', 'p0', '', 'l0', 'long one', 7, 'tag', 8, '', 104, 105]);
  assert.deepEqual(decodeMessage(s, Uint8Array.of(63), payload), {
    a: 'top',
    pair: ['p0', ''],
    list: ['l0', 'long one'],
    t: { v: 7, label: 'tag' },
    ts: [{ v: 8, label: '' }],
    c: [104, 105]
  });
  // Same size, other byte counts: a cached layout is reused only when every count matches.
  const changed = Uint8Array.from([
    ...str(ascii('to')), ...str(ascii('p0')), ...str(ascii('x')), ...payload.subarray(17)
  ]);
  assert.deepEqual(
      decodeFlat(s, Uint8Array.of(63), changed).values.slice(0, 3), ['to', 'p0', 'x']);
  error(
      'trailing_bytes',
      () => decodeFlat(s, Uint8Array.of(63), Uint8Array.from([...payload, 0])));
});
test(
    'a string keeps its BOM and NUL, replaces invalid UTF-8, and may change length',
    () => {
      const s = schema('string s\nuint8 after');
      for(const [input, expected] of [
              [[0xef, 0xbb, 0xbf, 0x61, 0, 0x62], '﻿a\0b'], [[], ''],
              [[0x61, 0xff], 'a�'], [[0xe2, 0x9c], '�'], [[0x68, 0x69], 'hi']])
        assert.deepEqual(
            decodeFlat(s, Uint8Array.of(3), Uint8Array.from([...str(input), 7])),
            { names: ['s', 'after'], values: [expected, 7] });
    });
test('a truncated string throws like the reference', () => {
  const s = schema('string s');
  for(const payload of [[], [3, 0, 0], [3, 0, 0, 0, 0x61], [...u32(0xffffffff), 0x61]])
    for(const decode of [decodeFlat, decodeMessage])
      error('truncated', () => decode(s, Uint8Array.of(1), Uint8Array.from(payload)));
  // string[] checks its count against 4 bytes per element first, string[N] as well.
  error(
      'truncated',
      () => decodeFlat(
          schema('string[] v'), Uint8Array.of(1),
          Uint8Array.from([...u32(2), ...str([])])));
  error(
      'truncated',
      () => decodeFlat(
          schema('string[3] v'), Uint8Array.of(1),
          Uint8Array.from([...str([]), ...str([])])));
});
test('nested YAML and text preserve fields and hash', () => {
  for(const name of ['schema_nested.txt', 'schema_nested.yaml'])
  {
    const s = parseSchema(text(name), { verifyHash: true });
    assert.equal(s.hash, 8022624192411902335n);
    assert.equal(s.fields[0].name, 'arm/joint_1/position');
    assert.equal(s.fields.length, 15);
  }
});
test('duplicate names, paths, arrays and unsafe property keys', () => {
  const s = schema('uint8 a\nuint8 b\nuint8 a');
  assert.deepEqual(
      decodeFlat(s, Uint8Array.of(7), Uint8Array.of(1, 2, 3)),
      { names: ['a', 'b'], values: [3, 2] });
  const paths = schema(
      'uint8 poses[3]/position/x\nuint8 __proto__/polluted\nuint8 constructor/prototype/x');
  const out = decodeMessage(paths, Uint8Array.of(7), Uint8Array.of(4, 5, 6));
  assert.equal(out.poses[3].position.x, 4);
  assert.equal(out.__proto__.polluted, 5);
  assert.equal({}.polluted, undefined);
  for(const fields
          of ['uint8 a\nuint8 a/b', 'uint8 a/b\nuint8 a', 'uint8 a[0]\nuint8 a/b'])
    error(
        'name_conflict',
        () => decodeMessage(schema(fields), Uint8Array.of(3), Uint8Array.of(1, 2)));
});
test('malformed categories and validation order', () => {
  error('bad_schema', () => parseSchema(''));
  error('missing_version', () => parseSchema('uint8 x'));
  error('unsupported_version', () => parseSchema('### version: 8'));
  error('bad_schema', () => schema('uint8[0] x'));
  error(
      'hash_mismatch',
      () => parseSchema('### version: 5\nuint8 x', { verifyHash: true }));
  const scalar = schema('uint8 x');
  error('truncated', () => decodeFlat(scalar, Uint8Array.of(1), new Uint8Array()));
  error('trailing_bytes', () => decodeFlat(scalar, Uint8Array.of(0), Uint8Array.of(1)));
  error('short_mask', () => decodeFlat(scalar, new Uint8Array(), new Uint8Array()));
  error('truncated', () => splitMcapMessage(new Uint8Array(7)));
  error('trailing_bytes', () => splitMcapMessage(new Uint8Array(9)));
  error(
      'unknown_type',
      () => decodeFlat(schema('Missing[] x'), Uint8Array.of(1), new Uint8Array(4)));
  const opaque = schema('O[] x\n' + sep + '\nMSG: O\nENCODING: foreign\nbody');
  assert.deepEqual(decodeFlat(opaque, Uint8Array.of(1), new Uint8Array(4)).names, []);
  error(
      'opaque_type',
      () => decodeFlat(opaque, Uint8Array.of(1), Uint8Array.of(1, 0, 0, 0)));
  error(
      'truncated',
      () => decodeFlat(
          schema('float64[] x'), Uint8Array.of(1), Uint8Array.of(255, 255, 255, 255)));
  const cyclic = schema(`A a\n${sep}\nMSG: A\nA[] a`);
  error(
      'too_deep',
      () => decodeFlat(
          cyclic, Uint8Array.of(1),
          Buffer.concat(Array.from({ length: 65 }, () => Buffer.from([1, 0, 0, 0])))));
  const registry = new SchemaRegistry();
  registry.add(7, '### version: 5\n### hash: 7\nuint8 x');
  error(
      'bad_count',
      () => decodeSnapshotMsg(
          registry,
          { schema_hash: 7, timestamp_nsec: 1, active_mask: [256], payload: [] }));
});
test('v4 hashes accepted unchecked', () => {
  for(const name of ['features', 'opaque', 'second'])
  {
    const s = parseSchema(text(`v4/schema_${name}.txt`), { verifyHash: true });
    assert.equal(typeof s.hash, 'bigint');
  }
});
test('ROS catalogs, embedded schemas, unknown hashes, exact timestamps', () => {
  const r = new SchemaRegistry();
  const entry = { hash: 7n, schema_text: '### version: 5\n### hash: 7\nuint8 x\n' };
  const msg = {
    schema_hash: 7,
    timestamp_nsec: 18446744073709551615n,
    active_mask: [1],
    payload: [42]
  };
  assert.equal(decodeSnapshotMsg(r, msg), undefined);
  r.addSchemas({ schemas: [entry] });
  const result = decodeSnapshotMsg(r, msg);
  assert.equal(result.timestamp_nsec, msg.timestamp_nsec);
  assert.deepEqual(result.values, { x: 42 });
  const r2 = new SchemaRegistry();
  assert.deepEqual(
      decodeSnapshotBatch(
          r2, { schemas: [entry], snapshots: [msg, {...msg, schema_hash: 8n }] }),
      [result]);
  assert.equal(r2.find(7), r2.find(7n));
  error('bad_hash', () => r.find(2 ** 60));
  error('hash_mismatch', () => r.add(8n, entry.schema_text));
});
test('floats preserve non-NaN bits and -0; all NaNs are equivalent', () => {
  const s = schema('float64[] q\nfloat64 x\nfloat32 f');
  const payload = Buffer.alloc(4 + 24 + 8 + 4);
  payload.writeUInt32LE(3);
  payload.writeBigUInt64LE(0x7ff0000000001234n, 4);
  payload.writeBigUInt64LE(0xfff8000000001234n, 12);
  payload.writeBigUInt64LE(0x8000000000000000n, 20);
  payload.writeBigUInt64LE(0x7ff0000000001234n, 28);
  payload.writeUInt32LE(0x7f801234, 36);
  const flat = decodeFlat(s, Uint8Array.of(7), payload);
  assert.deepEqual(
      flat.values.map(bits), ['nan', 'nan', '8000000000000000', 'nan', 'nan']);
  const msg = decodeMessage(s, Uint8Array.of(7), payload);
  assert.equal(bits(msg.q[0]), 'nan');
  assert.equal(bits(msg.x), 'nan');
});
test('ROS bytes reject sparse arrays, fractions, negatives and nonfinite values', () => {
  const registry = new SchemaRegistry();
  registry.add(7, '### version: 5\n### hash: 7\nuint8 x\n');
  for(const payload of [new Array(1), [undefined], [NaN], [Infinity], [-1], [1.5], [256]])
  {
    error(
        'bad_count',
        () => decodeSnapshotMsg(
            registry, { schema_hash: 7, timestamp_nsec: 0, active_mask: [1], payload }));
  }
  assert.equal(
      decodeSnapshotMsg(
          registry,
          { schema_hash: 8, timestamp_nsec: 0, active_mask: [], payload: new Array(1) }),
      undefined);
});
test('nested golden schemas decode identical enabled/disabled payloads', () => {
  for(const format of ['txt', 'yaml'])
  {
    const s = parseSchema(text(`schema_nested.${format}`), { verifyHash: true });
    const parts = [], values = [];
    for(const [i, f] of s.fields.slice(0, -1).entries())
    {
      const b = Buffer.alloc(f.type === 'float64' ? 8 : 1), value = i + 1;
      if(b.length === 8)
        b.writeDoubleLE(value);
      else
        b[0] = value;
      parts.push(b);
      values.push(value);
    }
    const pose = Buffer.alloc(28);
    [1, 2, 3].forEach((v, i) => pose.writeDoubleLE(v, i * 8));
    pose.writeUInt32LE(42, 24);
    const out = decodeFlat(s, Uint8Array.of(255, 127), Buffer.concat([...parts, pose]));
    assert.deepEqual(out.names, [
      ...s.fields.slice(0, -1).map(f => f.name), 'cart/pose/position/x',
      'cart/pose/position/y', 'cart/pose/position/z', 'cart/pose/stamp'
    ]);
    assert.deepEqual(out.values, [...values, 1, 2, 3, 42]);
    assert.deepEqual(decodeFlat(s, Uint8Array.of(0, 64), pose).values, [1, 2, 3, 42]);
    assert.deepEqual(
        decodeMessage(s, Uint8Array.of(0, 64), pose),
        { cart: { pose: { position: { x: 1, y: 2, z: 3 }, stamp: 42 } } });
  }
});
test('MCAP framing handles maximal lengths and offset subarray views', () => {
  for(const input
          of [Buffer.from('ffffffff', 'hex'), Buffer.from('00000000ffffffff', 'hex')])
    error('truncated', () => splitMcapMessage(input));
  const body = bytes('snapshot_full.mcap_message'),
        backing = Buffer.concat([Buffer.of(7), body, Buffer.of(9)]);
  const result = splitMcapMessage(backing.subarray(1, -1));
  assert.equal(result.payload.buffer, backing.buffer);
  assert.deepEqual(result.payload, bytes('snapshot_full.payload'));
});

test('schemaHash equals FNV-1a-64 computed with BigInt', () => {
  const reference = t => {
    const lines = t.split('\n'), at = lines.findIndex(l => l.startsWith('### hash:'));
    if(at >= 0)
      lines.splice(at, 1);
    let h = 0xcbf29ce484222325n;
    for(const b of new TextEncoder().encode(lines.join('\n')))
      h = BigInt.asUintN(64, (h ^ BigInt(b)) * 0x100000001b3n);
    return h;
  };
  const pieces = [
    'a', 'Z', '0', ' ', '\t', '\n', '\n### hash: 1', '### hash: 2\n', 'é', '€', '😀',
    '\u0085'
  ];
  let seed = 7;
  const next = () => (seed = (seed * 1103515245 + 12345) % 2147483648);
  for(let n = 0; n < 2000; n++)
  {
    let t = n % 3 === 0 ? '### hash: 9\n' : '';
    for(let k = next() % 64; k > 0; k--)
      t += pieces[next() % pieces.length];
    assert.equal(schemaHash(t), reference(t), JSON.stringify(t));
  }
});
