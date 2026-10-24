#!/usr/bin/env python3
"""Deterministic differential oracle. Run with python3 -I; emits JSON to stdout."""
import argparse
import json
import math
from pathlib import Path
import random
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'python'))
import data_tamer_parser as dt  # noqa: E402
from test_fast_path import random_schema, snapshot, variants, mask_of, encode  # noqa: E402

# The only Python-message -> TS-code table. An unrecognized error fails the oracle.
ERROR_CODES = [
    (r'truncated', 'truncated'),
    (r'trailing bytes', 'trailing_bytes'),
    (r'active mask shorter', 'short_mask'),
    (r'opaque encoding', 'opaque_type'),
    (r'unknown type', 'unknown_type'),
    (r'too deep|too deeply|cyclic schema', 'too_deep'),
    (r'no "### version:"', 'missing_version'),
    (r'unsupported .*version', 'unsupported_version'),
    (r'hash does not match', 'hash_mismatch'),
    (r'empty schema|invalid |out of range|without a name|expected |not right after|outside the start|bad |unterminated |unsupported YAML(?! schema version)|unexpected text|tab indentation|duplicate key|YAML schema:', 'bad_schema'),
]


def code(error):
    for pattern, mapped in ERROR_CODES:
        if re.search(pattern, str(error)):
            return mapped
    raise AssertionError(f'unmapped reference error: {error}')


class Text(str):
    """A "string" value, which TS decodes as a string; a char is a number there."""


class MarkingReader(dt._Reader):
    def text(self):
        return Text(super().text())


def bits(value):
    if isinstance(value, Text):
        return 's:' + value
    if isinstance(value, str):
        value = ord(value)
    return 'nan' if math.isnan(float(value)) else struct.pack('>d', float(value)).hex()


def nested(names, values):
    root = {}
    for name, value in zip(names, values):
        keys = []
        for segment in name.split('/'):
            match = re.fullmatch(r'(.*?)(\[[0-9]+\](?:\[[0-9]+\])*)', segment)
            if not match:
                keys.append(segment)
            else:
                keys.append(match[1])
                keys.extend(int(n) for n in re.findall(r'\[([0-9]+)\]', match[2]))
        node = root
        for index, key in enumerate(keys):
            array = isinstance(node, list)
            if array != isinstance(key, int):
                return {'messageError': 'name_conflict'}
            if array:
                while len(node) <= key:
                    node.append(None)
                old = node[key]
            else:
                old = node.get(key)
            if index == len(keys)-1:
                if isinstance(old, (list, dict)):
                    return {'messageError': 'name_conflict'}
                node[key] = value
            else:
                want_array = isinstance(keys[index+1], int)
                if old is None:
                    old = [] if want_array else {}
                    node[key] = old
                if not isinstance(old, (list, dict)) or isinstance(old, list) != want_array:
                    return {'messageError': 'name_conflict'}
                node = old
    return {'message': root}


def nested_reference(schema, mask, payload):
    # Flat dicts keep first insertion order: record all writes before deduplication
    # so numeric aliases (a[0], a[00]) still select the last payload value.
    class Writes:
        def __init__(self):
            self.names, self.values = [], []

        def __setitem__(self, name, value):
            self.names.append(name)
            self.values.append(bits(value))

    writes, reader = Writes(), MarkingReader(payload)
    for i, field in enumerate(schema.fields):
        if dt.get_bit(mask, i):
            dt._parse_field(field, schema, reader, '', writes)
    assert reader.pos == len(payload)
    return nested(writes.names, writes.values)


def same_values(a, b):
    """Equal value by value, NaN equal to NaN."""
    a, b = list(a), list(b)
    return len(a) == len(b) and all(x == y or (x != x and y != y) for x, y in zip(a, b))


def marked_reference(schema, mask, payload):
    """The reference walker's dict, with text values as Text."""
    out, reader = {}, MarkingReader(payload)
    for i, field in enumerate(schema.fields):
        if dt.get_bit(mask, i):
            dt._parse_field(field, schema, reader, '', out)
    assert reader.pos == len(payload)
    return out


def reference(text, samples, verify=False):
    group = {'text': text, 'verifyHash': verify, 'samples': []}
    try:
        schema = dt.parse_schema(text, verify_hash=verify)
    except ValueError as error:
        group['error'] = code(error)
        return group
    group['hash'] = str(schema.hash)
    group['computedHash'] = str(dt.schema_hash(dt.to_text(schema) if text.lstrip().startswith('version:') else text))
    for mask, payload in samples:
        case = {'mask': mask.hex(), 'payload': payload.hex()}
        outputs = []
        for decode in (dt._parse_snapshot_reference, dt.parse_snapshot):
            try:
                result = decode(schema, mask, payload)
                marked = marked_reference(schema, mask, payload)
                assert list(marked) == list(result) and same_values(marked.values(), result.values())
                outputs.append({'names': list(marked), 'bits': [bits(v) for v in marked.values()]})
            except ValueError as error:
                outputs.append({'error': code(error)})
        if outputs[0] != outputs[1]:
            raise AssertionError((text, case, outputs))
        if 'error' not in outputs[0]:
            names, values = dt.parse_snapshot_values(schema, mask, payload)
            assert list(names) == outputs[0]['names']
            assert same_values(values, marked.values())
        case.update(outputs[0])
        if 'error' not in case:
            case.update(nested_reference(schema, mask, payload))
        group['samples'].append(case)
    return group


def line_schema(fields, types=None, opaque=False):
    make = lambda fs: [dt.Field(name, t, n is not None, n or 0) for name, t, n in fs]
    schema = dt.Schema(channel_name='random', hash=7, fields=make(fields),
                       custom_types={name: make(fs) for name, fs in (types or {}).items()},
                       custom_schemas={'O': ('protobuf', '### hash: foreign\nMSG: body')} if opaque else {})
    return dt.to_text(schema)


def generate(seed, groups):
    rng = random.Random(seed)
    output = []
    for index in range(groups):
        schema = random_schema(rng)
        if index % 3 == 0:
            for f in schema.fields:
                f.field_name = rng.choice([f.field_name, '__proto__/x', 'constructor/prototype/x',
                                           'é', 'a[b][0]', 'a[b]', '/x//', '0'])
        samples = []
        for _ in range(2):
            mask, payload = snapshot(rng, schema)
            samples.extend(variants(rng, mask, payload))
        if payload:
            changed = bytearray(payload)
            changed[rng.randrange(len(changed))] ^= 1 << rng.randrange(8)
            samples.append((mask, bytes(changed)))
        if index % 17 == 0:
            payload = bytearray()
            for f in schema.fields:
                encode(rng, schema, f, payload)
            samples.append((mask_of([True] * len(schema.fields)), bytes(payload)))
        output.append(reference(dt.to_text(schema), samples))

    # Strings: random bytes, mostly not UTF-8, at the top level, in a vector of a struct
    # and in a fixed array; a char[2] and a char[] stay per character. (random_schema()
    # mixes "string" into every shape too.)
    strings = line_schema([('s', 'string', None), ('n', 'Named', 0), ('c', 'char', 2),
                           ('t', 'string', 2), ('h', 'char', 0)],
                          {'Named': [('k', 'string', None), ('v', 'uint8', None)]})
    text_bytes = lambda: rng.choice([
        rng.randbytes(rng.randint(0, 6)), 'Zürich ✓'.encode(), b'\xef\xbb\xbfbom',
        bytes(rng.choice([0x41, 0x80, 0xc3, 0xe2, 0xed, 0xf0, 0xf4, 0xff, 0xa0])
              for _ in range(rng.randint(1, 6)))])
    samples = []
    for _ in range(60):
        payload = bytearray()
        s = text_bytes()
        payload += struct.pack('<I', len(s)) + s
        items = rng.randint(0, 3)
        payload += struct.pack('<I', items)
        for _ in range(items):
            s = text_bytes()
            payload += struct.pack('<I', len(s)) + s + rng.randbytes(1)
        payload += rng.randbytes(2)
        for _ in range(2):
            s = text_bytes()
            payload += struct.pack('<I', len(s)) + s
        s = text_bytes()
        payload += struct.pack('<I', len(s)) + s
        samples.extend(variants(rng, b'\x1f', bytes(payload)))
    output.append(reference(strings, samples))
    for t in ['uint8', 'float64', 'Point', 'Nested', 'O', 'Undefined', 'Empty']:
        for count in [0, 1, 0xffffffff]:
            types = {'Point':[('x','float64',None)], 'Nested':[('v','uint8',0)], 'Empty':[]}
            output.append(reference(line_schema([('v',t,0)], types, True),
                                    [(b'\x01',struct.pack('<I',count)), (b'\x00',b'')]))
    for depth in [1, 2, 60, 63, 64, 65, 66, 80, 1200]:
        for vector in [False, True]:
            types = {f'T{i}': [('t',f'T{i+1}',0 if vector else None)] for i in range(depth-1)}
            types[f'T{depth-1}'] = [('x','float64',None)]
            body = (struct.pack('<I',1)*(depth-1) if vector else b'') + struct.pack('<d',-0.0)
            output.append(reference(line_schema([('root','T0',None)],types), [(b'\x01',body), (b'\x01',body[:-1])]))
    for count in [None, 0, 65535]:
        output.append(reference(line_schema([('a','A',count)], {'A':[('a','A',count)]}), [(b'\x01',b'\0'*4)]))
    for depth in [62, 63, 64]:
        types = {f'T{i}':[('t',f'T{i+1}',None)] for i in range(depth)}
        types[f'T{depth}'] = [('x','uint8',None)]
        output.append(reference(line_schema([('shallow',f'T{depth}',1), ('deep','T0',1)],types),
                                [(b'\x01',b'\x07'),(b'\x02',b'\x08'),(b'\x03',b'\x07\x08')]))
    recursive = line_schema([('roots', 'Node', 0)], {'Node': [('value', 'uint8', None), ('children', 'Node', 0)]})
    output.append(reference(recursive, [(b'\x01', bytes.fromhex('010000000700000000'))]))
    output.append(reference(line_schema([('a[0]', 'uint8', None), ('a[00]', 'uint8', None),
                                         ('a[0]', 'uint8', None)]), [(b'\x07', bytes([1, 2, 3]))]))
    malformed = ['', 'uint8 x', '### version: 3', '### version: -1', '### version: 18446744073709551616',
                 '### version: 5\nuint8 x y', '### version: 5\nuint8[0] x', '### version: 5\nuint8[65536] x',
                 '### version: 5\nuint8[1]junk x', '### version: 5\nMSG: A',
                 '### version: 5\n'+'='*30, '### version: 5\nENCODING: x']
    yaml = 'version: 5\nhash: 7\nchannel_name: "c"\nfields:\n  x: uint8\n'
    malformed += [yaml.replace('version: 5','version: 6'), yaml.replace('  x:', '\tx:'),
                  yaml+'  x: float64\n', yaml.replace('uint8','[uint8]'), yaml.replace('"c"','"\\uD800"'),
                  yaml.replace('"c"', '"\\U00110000"'), yaml.replace('uint8','uint8 # comment'),
                  yaml.replace('uint8',"'uint8'"), yaml.replace('  x:', '   x:')+'  y: uint8\n']
    output.extend(reference(text,[]) for text in malformed)
    output.append(reference(yaml, [(b'\x01',b'\x01')]))
    for whitespace in ['\u0085', '\u001c', '\ufeff', '\u200b']:
        output.append(reference(yaml.replace('\"c\"', '\"c\"' + whitespace), [(b'\x01',b'\x01')]))
    output.append(reference('### version: 5\n### hash: 1\n', [], True))
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--groups', type=int, default=650)
    parser.add_argument('--seed', type=int, default=0xDADA2026)
    args = parser.parse_args()
    json.dump({'errorCodes': ERROR_CODES, 'groups': generate(args.seed, args.groups)}, sys.stdout)


if __name__ == '__main__':
    main()
