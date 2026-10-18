import {bits, normalizeBits} from './helpers.mjs';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import mcap from '@mcap/core';
const { McapStreamReader } = mcap;
import {parseSchema, splitMcapMessage, decodeFlat} from '../dist/index.js';
const vectors = new URL('../../docs/wire_format/vectors/v4/', import.meta.url);

test('v4 1.0.4 recording: every expected.tsv row through @mcap/core', () => {
  const expected = new Map();
  for(const line of readFileSync(new URL('expected.tsv', vectors), 'utf8').split('\n'))
  {
    if(!line || line.startsWith('#'))
      continue;
    const [topic, time, name, value] = line.split('\t'), key = `${topic}:${time}`;
    if(!expected.has(key))
      expected.set(key, []);
    expected.get(key).push([name, value]);
  }
  const reader = new McapStreamReader(), schemas = new Map(), channels = new Map();
  reader.append(readFileSync(new URL('recorded_1.0.4.mcap', vectors)));
  let messages = 0, rejected = 0, rows = 0;
  for(let record; (record = reader.nextRecord());)
  {
    if(record.type === 'Schema')
    {
      const schema =
          parseSchema(new TextDecoder().decode(record.data), { verifyHash: true });
      assert.equal(record.name, `${schema.channelName}::${schema.hash}`);
      schemas.set(record.id, schema);
    }
    if(record.type === 'Channel')
      channels.set(record.id, record);
    if(record.type !== 'Message')
      continue;
    const channel = channels.get(record.channelId),
          schema = schemas.get(channel.schemaId);
    const key = `${channel.topic}:${record.logTime}`, entries = expected.get(key);
    assert.ok(entries, key);
    const { activeMask, payload } = splitMcapMessage(record.data);
    ++messages;
    if(entries[0][0] === '!undecodable')
    {
      assert.throws(
          () => decodeFlat(schema, activeMask, payload),
          { name: 'DataTamerError', code: 'opaque_type' });
      ++rejected;
    }
    else
    {
      const result = decodeFlat(schema, activeMask, payload);
      assert.deepEqual(result.names, entries.map(e => e[0]));
      assert.deepEqual(
          result.values.map(bits),
          entries.map(
              ([, v]) =>
                  v.startsWith('f:') ? normalizeBits(v.slice(2)) : bits(Number(v))));
      rows += entries.length;
    }
    expected.delete(key);
  }
  assert.ok(reader.done());
  assert.equal(expected.size, 0);
  assert.equal(messages, 15);
  console.log(`Recording: ${messages} messages, ${rows} numeric rows, ${
      rejected} opaque rejections`);
});
