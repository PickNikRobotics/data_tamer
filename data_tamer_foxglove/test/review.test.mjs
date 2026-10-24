import assert from 'node:assert/strict';
import test from 'node:test';
import {parseSchema, decodeFlat, decodeMessage} from '../dist/index.js';
import {schema, separator} from './helpers.mjs';

test(
    'finite recursive vectors decode; only the actual expansion is depth limited', () => {
      const s =
          schema(`Node[] roots\n${separator}\nMSG: Node\nuint8 value\nNode[] children`);
      const p = Buffer.from('010000000700000000', 'hex'), mask = Uint8Array.of(1);
      assert.deepEqual(
          decodeFlat(s, mask, p), { names: ['roots[0]/value'], values: [7] });
      assert.deepEqual(decodeMessage(s, mask, p), { roots: [{ value: 7 }] });
      const deep = Buffer.concat([
        Buffer.from('01000000', 'hex'),
        ...Array.from({ length: 65 }, () => Buffer.from('0701000000', 'hex')),
        Buffer.alloc(5)
      ]);
      assert.throws(
          () => decodeFlat(s, mask, deep), { name: 'DataTamerError', code: 'too_deep' });
      assert.deepEqual(decodeFlat(s, mask, p).values, [7]);
    });
test('original bracket text and appended indices retain distinct typed paths', () => {
  const s = schema(
      `uint8 a[b][0]\nuint8[2] c[d]\nP[1] poses\n${separator}\nMSG: P\nuint8 q[e][1]`);
  assert.deepEqual(decodeMessage(s, Uint8Array.of(7), Uint8Array.of(7, 8, 9, 10)), {
    'a[b]': [7],
    'c[d]': [8, 9],
    poses: [{ 'q[e]': [, 10] }],
  });
  const nested = schema('uint8[1] a[0]');
  assert.deepEqual(
      decodeMessage(nested, Uint8Array.of(1), Uint8Array.of(4)), { a: [[4]] });
});
test('YAML quoted remainder matches Python whitespace rather than JS trim', () => {
  const yaml = tail => `version: 5\nhash: 7\nchannel_name: "c"${tail}\nfields: {}\n`;
  for(const c of ['\u0085', '\u001c', '\u00a0', '\u2007'])
    assert.equal(parseSchema(yaml(c)).channelName, 'c');
  for(const c of ['\ufeff', '\u200b'])
    assert.throws(
        () => parseSchema(yaml(c)), { name: 'DataTamerError', code: 'bad_schema' });
});
test('depth memo checks cached height when the same type is reached deeper', () => {
  let fields = `A[] shallow\nC0 deep\n${separator}\nMSG: A\nB[2] b\n${
      separator}\nMSG: B\nB1 b\n${separator}\nMSG: B1\nB2 b\n${
      separator}\nMSG: B2\nB3 b\n${separator}\nMSG: B3\n`;
  for(let i = 0; i < 63; ++i)
    fields += `${separator}\nMSG: C${i}\n${i === 62 ? 'B[2] b' : `C${i + 1} c`}\n`;
  const s = schema(fields), p = Buffer.alloc(4);
  p.writeUInt32LE(1);
  decodeFlat(s, Uint8Array.of(1), p);
  assert.throws(
      () => decodeFlat(s, Uint8Array.of(3), p),
      { name: 'DataTamerError', code: 'too_deep' });
});

test('numeric aliases choose the last payload write after flat deduplication', () => {
  const s = schema('uint8 a[0]\nuint8 a[00]\nuint8 a[0]');
  const mask = Uint8Array.of(7), payload = Uint8Array.of(1, 2, 3);
  assert.deepEqual(
      decodeFlat(s, mask, payload), { names: ['a[0]', 'a[00]'], values: [3, 2] });
  assert.deepEqual(decodeMessage(s, mask, payload), { a: [3] });
});
test('raw-mask cache copies Buffer masks before callers can mutate them', () => {
  const s = schema('uint8 a\nuint8 b'), mask = Buffer.of(1), payload = Uint8Array.of(7);
  assert.deepEqual(decodeFlat(s, mask, payload).names, ['a']);
  mask[0] = 2;
  assert.deepEqual(decodeFlat(s, mask, payload).names, ['b']);
});
