export type ErrorCode = 'bad_schema' | 'missing_version' | 'unsupported_version'
  | 'hash_mismatch' | 'bad_hash' | 'truncated' | 'trailing_bytes' | 'short_mask'
  | 'opaque_type' | 'unknown_type' | 'too_deep' | 'bad_count' | 'name_conflict' | 'bad_timestamp';

export class DataTamerError extends Error {
  constructor(public readonly code: ErrorCode, message: string) {
    super(message);
    this.name = 'DataTamerError';
  }
}

export function fail(code: ErrorCode, message: string): never {
  throw new DataTamerError(code, message);
}
