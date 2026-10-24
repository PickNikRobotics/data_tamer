import { fail } from './errors.js';
import { parseSchema, type Schema, type SchemaOptions } from './schema.js';
import { decodeMessage, type Message } from './decode.js';

export type Hash = number | bigint;
export interface SchemaMsg { hash: Hash; schema_text: string; channel_name?: string }
export interface SchemasMsg { schemas: readonly SchemaMsg[] }
export interface SnapshotMsg {
  schema_hash: Hash;
  timestamp_nsec: number | bigint;
  active_mask: Uint8Array | readonly number[];
  payload: Uint8Array | readonly number[];
}
export interface SnapshotBatchMsg extends SchemasMsg { snapshots: readonly SnapshotMsg[] }
export interface DecodedSnapshot { schema: Schema; timestamp_nsec: number | bigint; values: Message }
function hashKey(hash: Hash): bigint {
  if (typeof hash !== 'bigint' && (typeof hash !== 'number' || !Number.isSafeInteger(hash)))
    fail('bad_hash', 'hash must be bigint or an exact safe integer');
  const value = BigInt(hash);
  if (value < 0n || value > 0xffffffffffffffffn) fail('bad_hash', 'hash must fit uint64');
  return value;
}
export class SchemaRegistry {
  private readonly schemas = new Map<bigint, Schema>();
  constructor(private readonly options: SchemaOptions = {}) {}
  add(hash: Hash, text: string): Schema {
    const key = hashKey(hash), known = this.schemas.get(key);
    if (known) return known;
    const schema = parseSchema(text, this.options);
    if (schema.hash !== key) fail('hash_mismatch', `schema text declares hash ${schema.hash}, expected ${key}`);
    this.schemas.set(key,schema);
    return schema;
  }
  addSchemas(msg: SchemasMsg): void {
    for (const entry of msg.schemas) this.add(entry.hash,entry.schema_text);
  }
  find(hash: Hash): Schema | undefined { return this.schemas.get(hashKey(hash)); }
}
function bytes(value: Uint8Array | readonly number[]): Uint8Array {
  if (value instanceof Uint8Array) return value;
  if (!Array.isArray(value)) fail('bad_count', 'expected a byte array');
  for (const n of value)
    if (!Number.isInteger(n) || n < 0 || n > 255)
      fail('bad_count', 'byte array elements must be integers in 0..255');
  return Uint8Array.from(value);
}
export function decodeSnapshotMsg(registry: SchemaRegistry, msg: SnapshotMsg): DecodedSnapshot | undefined {
  const schema = registry.find(msg.schema_hash);
  if (!schema) return undefined;
  const mask = bytes(msg.active_mask), payload = bytes(msg.payload);
  return {schema, timestamp_nsec:msg.timestamp_nsec, values:decodeMessage(schema,mask,payload)};
}
export function decodeSnapshotBatch(registry: SchemaRegistry, batch: SnapshotBatchMsg): DecodedSnapshot[] {
  registry.addSchemas(batch);
  const results: DecodedSnapshot[] = [];
  for (const msg of batch.snapshots) {
    const result = decodeSnapshotMsg(registry,msg);
    if (result) results.push(result);
  }
  return results;
}
