import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import {parseSchema, schemaHash, decodeFlat, decodeMessage} from '../dist/index.js';
import {bits, messageBits} from './helpers.mjs';
test('Python walker and fast path: flat AND nested differential', () => {
  const fixture = JSON.parse(
      readFileSync(new URL('../dist/reference.json', import.meta.url), 'utf8'));
  let count = 0, errors = 0, messages = 0, conflicts = 0, perCharacter = 0;
  for(const [index, group] of fixture.groups.entries())
  {
    if(group.error)
    {
      assert.throws(
          () => parseSchema(group.text, { verifyHash: group.verifyHash }),
          { name: 'DataTamerError', code: group.error });
      ++errors;
      continue;
    }
    const schema = parseSchema(group.text, { verifyHash: group.verifyHash });
    assert.equal(String(schema.hash), group.hash);
    assert.equal(String(schemaHash(group.text)), group.computedHash);
    for(const sample of group.samples)
    {
      ++count;
      const mask = Buffer.from(sample.mask, 'hex'),
            payload = Buffer.from(sample.payload, 'hex');
      if(sample.error)
      {
        for(const decode of [decodeFlat, decodeMessage])
          assert.throws(
              () => decode(schema, mask, payload),
              { name: 'DataTamerError', code: sample.error }, `${index}: ${group.text}`);
        ++errors;
        continue;
      }
      const out = decodeFlat(schema, mask, payload);
      assert.deepEqual(out.names, sample.names, `${index}: names`);
      assert.deepEqual(
          out.values.map(bits), sample.bits,
          `${index}: ${group.text}\n${sample.payload}`);
      assert.equal(decodeFlat(schema, mask, payload).names, out.names);
      if(sample.chars)
      {
        const chars = decodeFlat(schema, mask, payload, { stringsAsText: false });
        assert.deepEqual(
            chars.names, sample.chars.names, `${index}: per-character names`);
        assert.deepEqual(
            chars.values.map(bits), sample.chars.bits, `${index}: per character`);
        ++perCharacter;
      }
      if(sample.messageError)
      {
        assert.throws(
            () => decodeMessage(schema, mask, payload),
            { name: 'DataTamerError', code: sample.messageError });
        ++conflicts;
      }
      else
      {
        assert.deepEqual(
            messageBits(decodeMessage(schema, mask, payload)), sample.message,
            `${index}: nested message`);
        ++messages;
      }
    }
  }
  assert.ok(
      messages > 0 && conflicts > 0,
      'both nested objects and conflicts must be exercised');
  assert.ok(perCharacter > 0, 'strings must also be decoded per character');
  console.log(`Differential: ${fixture.groups.length} schemas, ${count} payloads, ${
      errors} expected errors, ${messages} nested objects, ${conflicts} name conflicts`);
});
