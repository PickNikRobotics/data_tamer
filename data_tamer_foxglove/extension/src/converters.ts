import type { Immutable, MessageEvent, Time } from '@foxglove/extension';
import {
  DataTamerError, SchemaRegistry, decodeSnapshotMsg,
  type Hash, type Message, type SchemaMsg, type SchemasMsg, type SnapshotMsg, type SnapshotBatchMsg,
} from '../../src/index';

export function createConverterState(warn: (...args: unknown[]) => void = console.warn) {
  return {
    registry: new SchemaRegistry({ verifyHash: true }),
    rejectedSchemas: new Map<string, { hash: Hash; hashDependent: boolean; error: unknown }>(),
    warned: new Set<string>(),
    warn,
  };
}
type ConverterState = ReturnType<typeof createConverterState>;

const sharedState = createConverterState();
type Event = Immutable<MessageEvent>;
type Snapshot = {
  header: { stamp: Time; frame_id: string };
  channel: string;
  values: Message;
};

function report(error: unknown, topic: string, state: ConverterState, hash?: Hash): void {
  const code = error instanceof DataTamerError ? error.code : 'invalid_message';
  const id = hash === undefined ? undefined : String(hash);
  const key = JSON.stringify([topic, id, code]);
  if (state.warned.has(key)) return;
  state.warned.add(key);
  state.warn(`[data_tamer] ${topic}${id === undefined ? '' : ` (schema ${id})`}: ${code}`, error);
}

function guard<T>(event: Event, state: ConverterState, fn: () => T): T | undefined {
  try {
    return fn();
  } catch (error) {
    report(error, event.topic, state);
    return undefined;
  }
}

function addSchema(entry: SchemaMsg, state: ConverterState) {
  const text = entry?.schema_text;
  const rejected = state.rejectedSchemas.get(text);
  // A corrected message hash must be able to register otherwise valid text.
  if (rejected && (!rejected.hashDependent || rejected.hash === entry.hash)) throw rejected.error;
  try {
    const schema = state.registry.add(entry.hash, text);
    state.rejectedSchemas.delete(text);
    return schema;
  } catch (error) {
    if (typeof text === 'string') {
      const hashDependent = error instanceof DataTamerError
        && (error.code === 'bad_hash' || error.code === 'hash_mismatch');
      state.rejectedSchemas.set(text, { hash: entry.hash, hashDependent, error });
      if (state.rejectedSchemas.size > 256)
        state.rejectedSchemas.delete(state.rejectedSchemas.keys().next().value!);
    }
    throw error;
  }
}

export function timestampToStamp(timestamp: number | bigint): Time {
  if (typeof timestamp !== 'bigint' && (typeof timestamp !== 'number' || !Number.isInteger(timestamp)))
    throw new DataTamerError('bad_timestamp', 'timestamp_nsec must be an integer number or bigint');
  const ns = BigInt(timestamp);
  if (ns < 0n || ns > 0xffffffffffffffffn)
    throw new DataTamerError('bad_timestamp', 'timestamp_nsec must fit uint64');
  return { sec: Number(ns / 1000000000n), nsec: Number(ns % 1000000000n) };
}

export function convertSchemas(message: unknown, event: Event, state = sharedState) {
  return guard(event, state, () => {
    const schemas: { channel: string; hash: string; fields: number }[] = [];
    let errors = 0;
    for (const entry of (message as SchemasMsg).schemas) {
      try {
        const schema = addSchema(entry, state);
        schemas.push({ channel: schema.channelName, hash: schema.hash.toString(), fields: schema.fields.length });
      } catch (error) {
        errors++;
        report(error, event.topic, state, entry?.hash);
      }
    }
    return { schemas, errors };
  });
}

export function convertSnapshot(message: unknown, event: Event, state = sharedState): Snapshot | undefined {
  return guard(event, state, () => {
    const decoded = decodeSnapshotMsg(state.registry, message as SnapshotMsg);
    if (!decoded) return undefined;
    return {
      header: { stamp: timestampToStamp(decoded.timestamp_nsec), frame_id: '' },
      channel: decoded.schema.channelName,
      values: decoded.values,
    };
  });
}

export function convertSnapshotBatch(message: unknown, event: Event, state = sharedState) {
  return guard(event, state, () => {
    const batch = message as SnapshotBatchMsg;
    if (!convertSchemas(batch, event, state)) return undefined;
    const snapshots: Snapshot[] = [];
    // Decode separately so one corrupt/unknown snapshot does not discard its siblings.
    for (const input of batch.snapshots) {
      const snapshot = convertSnapshot(input, event, state);
      if (snapshot) snapshots.push(snapshot);
    }
    return { snapshots };
  });
}
