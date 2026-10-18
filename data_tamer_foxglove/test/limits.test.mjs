import {cacheStats, CACHE_LIMITS} from '../dist/decode.js';
import {schema, separator as sep} from './helpers.mjs';
import assert from 'node:assert/strict';
import test from 'node:test';
import {decodeFlat, decodeMessage} from '../dist/index.js';

function vector(count)
{
  const payload = Buffer.alloc(4 + count);
  payload.writeUInt32LE(count);
  return payload;
}

test('normalized masks ignore high bits/bytes and observe caller mutations', () => {
  const s = schema('uint8 a\nuint8 b'), p = Uint8Array.of(7), m = Uint8Array.of(1);
  const names = decodeFlat(s, m, p).names;
  assert.equal(decodeFlat(s, Uint8Array.of(253, 255, 255), p).names, names);
  m[0] = 2;
  assert.deepEqual(decodeFlat(s, m, p).names, ['b']);
  assert.equal(cacheStats(s).masks, 2);
});
test('many masks and layouts stay within both cache budgets', () => {
  const s = schema(Array.from({ length: 8 }, (_, i) => `uint8 x${i}`).join('\n'));
  for(let mask = 0; mask < 256; ++mask)
  {
    const payload = new Uint8Array(mask.toString(2).replaceAll('0', '').length);
    decodeFlat(s, Uint8Array.of(mask), payload);
    assert.ok(cacheStats(s).masks <= CACHE_LIMITS.masks);
  }
  const v = schema('uint8[] v');
  const first = decodeFlat(v, Uint8Array.of(1), vector(1)).names;
  const second = decodeFlat(v, Uint8Array.of(1), vector(2)).names;
  assert.equal(decodeFlat(v, Uint8Array.of(1), vector(1)).names, first);
  assert.equal(decodeFlat(v, Uint8Array.of(1), vector(2)).names, second);
  for(let count = 3; count < 150; ++count)
  {
    decodeMessage(v, Uint8Array.of(1), vector(count));
    assert.ok(cacheStats(v).layouts <= CACHE_LIMITS.vectorLayouts);
    assert.ok(cacheStats(v).names <= CACHE_LIMITS.names);
  }
  decodeFlat(v, Uint8Array.of(1), vector(CACHE_LIMITS.names + 1));
  assert.ok(cacheStats(v).names <= CACHE_LIMITS.names);
});
test(
    'counts are revalidated after warming the cache, including equal-size layouts',
    () => {
      const s = schema(`N[] v\n${sep}\nMSG: N\nuint8[] a`);
      const payload = Buffer.from([2, 0, 0, 0, 1, 0, 0, 0, 7, 0, 0, 0, 0]);
      assert.deepEqual(decodeFlat(s, Uint8Array.of(1), payload).values, [7]);
      payload.writeUInt32LE(0xffffffff, 4);
      assert.throws(
          () => decodeFlat(s, Uint8Array.of(1), payload),
          { name: 'DataTamerError', code: 'truncated' });
    });
test(
    'empty types skip hostile counts and large arrays without looping', { timeout: 1000 },
    () => {
      for(const spec of ['Empty[]', 'Empty[65535]'])
      {
        const s = schema(`${spec} v\n${sep}\nMSG: Empty`);
        const payload =
            spec.includes('[]') ? Buffer.from([255, 255, 255, 255]) : Buffer.alloc(0);
        assert.deepEqual(
            decodeFlat(s, Uint8Array.of(1), payload), { names: [], values: [] });
      }
    });
test(
    'exponentially expanding empty scalar types take linear time', { timeout: 1000 },
    () => {
      let text = 'T0 root\n';
      for(let i = 0; i < 40; ++i)
        text += `${sep}\nMSG: T${i}\nT${i + 1} a\nT${i + 1} b\n`;
      text += `${sep}\nMSG: T40\n`;
      assert.deepEqual(
          decodeFlat(schema(text), Uint8Array.of(1), new Uint8Array()).names, []);
    });
test('depth limit accepts 64 and rejects 65 regardless of memo history', () => {
  for(const n of [63, 64, 65, 1200])
  {
    let text = 'T0 deep\n';
    for(let i = 0; i < n; ++i)
      text += `${sep}\nMSG: T${i}\n${i === n - 1 ? 'uint8 x' : `T${i + 1} t`}\n`;
    if(n > 64)
      assert.throws(
          () => decodeFlat(schema(text), Uint8Array.of(1), Uint8Array.of(7)),
          { name: 'DataTamerError', code: 'too_deep' });
    else
      assert.deepEqual(
          decodeFlat(schema(text), Uint8Array.of(1), Uint8Array.of(7)).values, [7]);
  }
});
test('unsafe 64-bit payload integers round correctly at halfway boundaries', () => {
  const s = schema('int64[] v\nuint64[] u'), p = Buffer.alloc(8 + 8 * 8);
  p.writeUInt32LE(4);
  p.writeUInt32LE(4, 36);
  const signed = [-(1n << 63n), -9007199254740993n, -9007199254740995n, (1n << 63n) - 1n];
  const unsigned =
      [9007199254740993n, 9007199254740995n, 0xffffffffffffffffn, 0xfffffffffffff7ffn];
  signed.forEach((v, i) => p.writeBigInt64LE(v, 4 + i * 8));
  unsigned.forEach((v, i) => p.writeBigUInt64LE(v, 40 + i * 8));
  assert.deepEqual(
      decodeFlat(s, Uint8Array.of(3), p).values, [...signed, ...unsigned].map(Number));
});
test('deep slash paths do not use the JavaScript call stack', () => {
  const name = Array(10000).fill('a').join('/');
  const s = schema(`uint8 ${name}`);
  let result = decodeMessage(s, Uint8Array.of(1), Uint8Array.of(9));
  for(let i = 1; i < 10000; ++i)
    result = result.a;
  assert.equal(result.a, 9);
});
