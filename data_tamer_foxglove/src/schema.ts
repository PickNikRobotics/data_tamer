import { fail } from './errors.js';

export const MAX_SCHEMA_DEPTH = 64;

export interface Field {
  readonly name: string;
  /** A basic type (basicSizes, or "string": a uint32 byte count, then UTF-8 bytes) or a custom type. */
  readonly type: string;
  /** null: scalar; 0: dynamic vector; positive: fixed array. */
  readonly count: number | null;
}
export interface Schema {
  readonly version: number;
  readonly channelName: string;
  readonly hash: bigint;
  readonly fields: readonly Field[];
  readonly customTypes: Readonly<Record<string, readonly Field[]>>;
  readonly opaqueTypes: Readonly<Record<string, readonly [string, string]>>;
}
export interface SchemaOptions { verifyHash?: boolean }
export const basicSizes: Readonly<Record<string, number>> = Object.freeze(Object.assign(Object.create(null), {
  bool: 1, char: 1, int8: 1, uint8: 1, int16: 2, uint16: 2,
  int32: 4, uint32: 4, int64: 8, uint64: 8, float32: 4, float64: 8,
}));
const trim = (s: string): string => s.replace(/^[ \r]+|[ \r]+$/g, '');
const validName = (s: string): boolean => s.length > 0 && !/[\x00-\x20\x7f]/.test(s);
function uint(s: string, what: string): bigint {
  if (!/^[0-9]{1,20}$/.test(s) || BigInt(s) > 0xffffffffffffffffn)
    fail('bad_schema', `invalid ${what}: ${s}`);
  return BigInt(s);
}
function field(spec: string, name: string): Field {
  if (!validName(name)) fail('bad_schema', `invalid field name: ${name}`);
  const bracket = spec.indexOf('['), type = bracket < 0 ? spec : spec.slice(0, bracket);
  if (!type) fail('bad_schema', 'empty type');
  if (bracket < 0) return {name, type, count: null};
  const inner = spec.slice(bracket + 1, -1);
  if (!spec.endsWith(']') || !/^[0-9]*$/.test(inner)) fail('bad_schema', `invalid type: ${spec}`);
  const count = inner ? Number(inner) : 0;
  if (inner && (count < 1 || count > 65535)) fail('bad_schema', `array size out of range: ${spec}`);
  return {name, type, count};
}

/** FNV-1a-64. YAML is hashed through its canonical v5 line rendering. */
export function schemaHash(text: string): bigint {
  if (firstLine(text).startsWith('version:')) return hashText(toText(parseSchema(text)));
  return hashText(text);
}
function hashText(text: string): bigint {
  // The text without its first "### hash:" line, as the writer hashes it.
  let body = text;
  const at = text.startsWith('### hash:') ? 0 : text.indexOf('\n### hash:') + 1;
  if (at > 0 || text.startsWith('### hash:')) {
    const end = text.indexOf('\n', at);
    body = at === 0 ? (end < 0 ? '' : text.slice(end + 1)) : text.slice(0, at - 1) + (end < 0 ? '' : text.slice(end));
  }
  // The 64-bit state as two 32-bit halves. The prime is 2^40 + 0x1b3, so the multiply is
  // lo * 0x1b3 (its carry into hi), hi * 0x1b3, and lo << 8 into hi: every partial product
  // stays below 2^53, exact in doubles, without a BigInt per byte.
  const bytes = new TextEncoder().encode(body);
  let hi = 0xcbf29ce4, lo = 0x84222325;
  for (let i = 0; i < bytes.length; i++) {
    lo = (lo ^ bytes[i]!) >>> 0;
    const low = lo * 0x1b3;
    hi = (hi * 0x1b3 + Math.floor(low / 0x100000000) + ((lo << 8) >>> 0)) >>> 0;
    lo = low >>> 0;
  }
  return (BigInt(hi) << 32n) | BigInt(lo);
}
function firstLine(text: string): string {
  return text.split('\n').map(trim).find(l => l && (!l.startsWith('#') || l.startsWith('###'))) ?? '';
}
export function parseSchema(text: string, options: SchemaOptions = {}): Schema {
  if (typeof text !== 'string') fail('bad_schema', 'schema text must be a string');
  const yaml = firstLine(text).startsWith('version:');
  const parsed = yaml ? parseYaml(text) : parseLines(text);
  if (options.verifyHash && parsed.version !== 4 && parsed.hash !== hashText(yaml ? toText(parsed) : text))
    fail('hash_mismatch', 'schema hash does not match its text');
  for (const fields of [parsed.fields, ...Object.values(parsed.customTypes)]) {
    fields.forEach(Object.freeze);
    Object.freeze(fields);
  }
  Object.values(parsed.opaqueTypes).forEach(Object.freeze);
  Object.freeze(parsed.customTypes);
  Object.freeze(parsed.opaqueTypes);
  return Object.freeze(parsed);
}
function parseLines(text: string): Schema {
  const fields: Field[] = [], customTypes: Record<string, Field[]> = Object.create(null);
  const opaqueTypes: Record<string, [string, string]> = Object.create(null);
  const lines = text.split('\n');
  let target = fields, version = 0, hash = 0n, channelName = '', previous = '', lastType = '', seen = false;
  for (let i = 0; i < lines.length; ++i) {
    const line = trim(lines[i]);
    if (!line) continue;
    seen = true;
    const afterSeparator = previous === 'separator', opensSection = previous === 'MSG';
    if (afterSeparator && !line.startsWith('MSG: ')) fail('bad_schema', 'expected MSG after separator');
    previous = '';
    if (/^={30,}$/.test(line)) { previous = 'separator'; continue; }
    const colon = line.indexOf(':'), key = line.slice(0, colon), value = trim(line.slice(colon + 1));
    if (colon >= 0 && key === '### version') {
      const n = uint(value, 'version');
      if (n !== 4n && n !== 5n) fail('unsupported_version', `unsupported schema version: ${value}`);
      version = Number(n);
    } else if (colon >= 0 && key === '### hash') hash = uint(value, 'hash');
    else if (colon >= 0 && key === '### channel_name') channelName = value;
    else if (line.startsWith('MSG: ')) {
      if (!afterSeparator) fail('bad_schema', 'MSG not after separator');
      lastType = trim(line.slice(5));
      target = customTypes[lastType] ??= [];
      previous = 'MSG';
    } else if (line.startsWith('ENCODING: ')) {
      if (!opensSection) fail('bad_schema', 'ENCODING outside section start');
      const foreign = lines.slice(i + 1).join('\n');
      opaqueTypes[lastType] = [trim(line.slice(10)), foreign.endsWith('\n') ? foreign.slice(0, -1) : foreign];
      delete customTypes[lastType];
      break;
    } else {
      if (!version) fail('missing_version', 'no ### version: line before fields; supported versions: 4 and 5');
      const space = line.indexOf(' ');
      if (space < 0) fail('bad_schema', 'field line without a name');
      target.push(field(line.slice(0, space), trim(line.slice(space + 1))));
    }
  }
  if (!seen) fail('bad_schema', 'empty schema text');
  if (!version) fail('missing_version', 'no ### version: line; supported versions: 4 and 5');
  if (previous === 'separator') fail('bad_schema', 'expected MSG after last separator');
  return {version, hash, channelName, fields, customTypes, opaqueTypes};
}

// The writer's YAML subset, deliberately independent of general YAML loaders.
type Node = {key: string; value: string | null; children: Node[] | null};
function quoted(s: string): [string, string] {
  const escapes: Record<string, string> = {'"':'"', '\\':'\\', '/':'/', n:'\n', t:'\t', r:'\r', '0':'\0'};
  let out = '';
  for (let i = 1; i < s.length;) {
    const c = s[i++];
    if (c === '"') return [out, s.slice(i)];
    if (c !== '\\') { out += c; continue; }
    const e = s[i++];
    if (Object.hasOwn(escapes, e)) { out += escapes[e]; continue; }
    const n = ({x:2, u:4, U:8} as Record<string, number>)[e];
    const digits = s.slice(i, i + (n ?? 0));
    if (!n || digits.length !== n || !/^[0-9a-fA-F]+$/.test(digits)) fail('bad_schema', 'bad YAML escape');
    const cp = parseInt(digits, 16);
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) fail('bad_schema', 'bad YAML escape');
    out += String.fromCodePoint(cp);
    i += n;
  }
  return fail('bad_schema', 'unterminated YAML string');
}
function scalar(s: string): string {
  if (s.startsWith('"')) {
    const [value, rest] = quoted(s);
    // Python str.strip includes U+0085 and U+001C..1F, but excludes U+FEFF.
    if (/[^\u0009-\u000d\u001c-\u0020\u0085\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]/.test(rest)) fail('bad_schema', 'unexpected text after YAML string');
    return value;
  }
  if (/^['\[{&*!|>\-?]/.test(s) || s.includes(' #')) fail('bad_schema', 'unsupported YAML');
  return s;
}
function yamlTree(text: string): Node[] {
  const root: Node[] = [];
  const stack: {parent: number; indent: number | null; entries: Node[]; keys: Set<string>}[] = [
    {parent:-1, indent:0, entries:root, keys:new Set()},
  ];
  for (const raw of text.split('\n')) {
    const line = raw.replace(/[ \r]+$/g, ''), content = line.replace(/^ +/, '');
    if (!content || content.startsWith('#')) continue;
    if (content.startsWith('\t')) fail('bad_schema', 'tab indentation');
    const indent = line.length - content.length;
    while (indent <= stack[stack.length - 1].parent) stack.pop();
    const level = stack[stack.length - 1];
    if (level.indent === null) level.indent = indent;
    if (level.indent !== indent) fail('bad_schema', 'bad YAML indentation');
    let key: string, rest: string;
    if (content.startsWith('"')) {
      [key, rest] = quoted(content);
      if (!rest.startsWith(':')) fail('bad_schema', "expected ':' after key");
      rest = rest.slice(1);
    } else {
      let colon = content.indexOf(': ');
      if (colon < 0) {
        if (!content.endsWith(':')) fail('bad_schema', 'expected key: value');
        colon = content.length - 1;
      }
      key = scalar(content.slice(0, colon));
      rest = content.slice(colon + 1);
    }
    if (rest && !rest.startsWith(' ')) fail('bad_schema', "expected a space after ':'");
    rest = rest.replace(/^ +| +$/g, '');
    if (level.keys.has(key)) fail('bad_schema', `duplicate YAML key: ${key}`);
    level.keys.add(key);
    const children: Node[] | null = rest === '' || rest === '{}' ? [] : null;
    level.entries.push({key, children, value: children === null ? scalar(rest) : null});
    if (rest === '') {
      if (stack.length > MAX_SCHEMA_DEPTH) fail('too_deep', 'YAML nesting too deep');
      stack.push({parent:indent, indent:null, entries:children!, keys:new Set()});
    }
  }
  return root;
}
function flatten(nodes: Node[], prefix = '', out: Field[] = []): Field[] {
  for (const node of nodes) {
    if (node.children === null) out.push(field(node.value!, prefix + node.key));
    else flatten(node.children, prefix + node.key + '/', out);
  }
  return out;
}
function parseYaml(text: string): Schema {
  const top = new Map(yamlTree(text).map(n => [n.key, n]));
  const getScalar = (key: string): string => {
    const n = top.get(key);
    if (!n || n.value === null) return fail('bad_schema', `YAML schema missing ${key}`);
    return n.value;
  };
  const mapping = (key: string, required = false): Node[] => {
    const n = top.get(key);
    if (!n && !required) return [];
    if (!n || n.children === null) return fail('bad_schema', `YAML schema requires mapping ${key}`);
    return n.children;
  };
  if (uint(getScalar('version'), 'version') !== 5n) fail('unsupported_version', 'unsupported YAML schema version');
  const customTypes: Record<string, Field[]> = Object.create(null);
  const opaqueTypes: Record<string, [string, string]> = Object.create(null);
  for (const n of mapping('types')) {
    if (n.children === null) fail('bad_schema', 'YAML type must be a mapping');
    customTypes[n.key] = flatten(n.children);
  }
  for (const n of mapping('opaque_types')) {
    const entry = new Map((n.children ?? []).map(e => [e.key, e.value]));
    const encoding = entry.get('encoding'), body = entry.get('schema');
    if (encoding == null || body == null) fail('bad_schema', 'opaque type needs encoding and schema');
    opaqueTypes[n.key] = [encoding, body];
  }
  return {version:5, hash:uint(getScalar('hash'), 'hash'), channelName:getScalar('channel_name'),
    fields:flatten(mapping('fields', true)), customTypes, opaqueTypes};
}
function toText(s: Schema): string {
  const line = (f: Field): string => `${f.type}${f.count === null ? '' : `[${f.count || ''}]`} ${f.name}\n`;
  let out = `### version: 5\n### hash: ${s.hash}\n### channel_name: ${s.channelName}\n\n` + s.fields.map(line).join('');
  // Python sorts Unicode code points, whereas JS's default sorts UTF-16 code units.
  const sorted = (keys: string[]): string[] => keys.sort((a,b) => {
    const aa = Array.from(a, c => c.codePointAt(0)!), bb = Array.from(b, c => c.codePointAt(0)!);
    for (let i = 0; i < Math.min(aa.length,bb.length); ++i) if (aa[i] !== bb[i]) return aa[i] - bb[i];
    return aa.length - bb.length;
  });
  for (const type of sorted(Object.keys(s.customTypes)))
    out += '='.repeat(59) + `\nMSG: ${type}\n` + s.customTypes[type].map(line).join('');
  for (const type of sorted(Object.keys(s.opaqueTypes))) {
    const [encoding, body] = s.opaqueTypes[type];
    out += '='.repeat(59) + `\nMSG: ${type}\nENCODING: ${encoding}\n${body}\n`;
  }
  return out;
}
