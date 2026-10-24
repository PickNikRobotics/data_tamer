import {parseSchema} from '../dist/index.js';
export const separator = '='.repeat(59);
export const schema = fields => parseSchema(`### version: 5\n### hash: 7\n${fields}\n`);
// NaNs compare by classification; every other number compares by binary64 bits, and
// a string (a "string" value) by its text.
export const bits = n => {
  if(typeof n === 'string')
    return `s:${n}`;
  if(Number.isNaN(n))
    return 'nan';
  const b = Buffer.alloc(8);
  b.writeDoubleBE(n);
  return b.toString('hex');
};
export const normalizeBits = hex => bits(Buffer.from(hex, 'hex').readDoubleBE());
export const messageBits = value => {
  if(typeof value === 'number' || typeof value === 'string')
    return bits(value);
  if(Array.isArray(value))
    return Array.from(value, v => v === undefined ? null : messageBits(v));
  return Object.fromEntries(
      Object.entries(value).map(([key, v]) => [key, messageBits(v)]));
};
