"""Reference decoder for the Data Tamer wire format.

Pure standard library. The format is specified in docs/wire_format.md; this
module is kept deliberately literal so it can be ported to another language
line by line. Usage:

    schema = parse_schema(schema_text)
    values = parse_snapshot(schema, active_mask, payload)   # {"pose/position/x": 1.0, ...}

For an MCAP message body written by MCAPSink use split_mcap_message() first.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Dict, List, Tuple

SCHEMA_VERSION = 4

# BasicType names in enum order; the index is the wire type id.
BASIC_TYPES = ["bool", "char", "int8", "uint8", "int16", "uint16", "int32",
               "uint32", "int64", "uint64", "float32", "float64"]

# struct format (little-endian) and size in bytes for each basic type
_STRUCT = {
    "bool": ("<?", 1), "char": ("<c", 1),
    "int8": ("<b", 1), "uint8": ("<B", 1),
    "int16": ("<h", 2), "uint16": ("<H", 2),
    "int32": ("<i", 4), "uint32": ("<I", 4),
    "int64": ("<q", 8), "uint64": ("<Q", 8),
    "float32": ("<f", 4), "float64": ("<d", 8),
}


@dataclass
class Field:
    name: str
    type_name: str          # one of BASIC_TYPES, or a custom type name
    is_vector: bool = False  # true for "T[]" and "T[N]"
    array_size: int = 0     # N for "T[N]", 0 for "T[]" (size prefix on the wire)

    @property
    def is_basic(self) -> bool:
        return self.type_name in _STRUCT


@dataclass
class Schema:
    channel_name: str = ""
    hash: int = 0
    fields: List[Field] = field(default_factory=list)
    custom_types: Dict[str, List[Field]] = field(default_factory=dict)
    # opaque custom encodings: type name -> (encoding, schema text)
    custom_schemas: Dict[str, Tuple[str, str]] = field(default_factory=dict)


def _parse_field_line(line: str) -> Field:
    type_part, _, name = line.partition(" ")
    name = name.strip()
    if not name:
        raise ValueError(f"field line without a name: {line!r}")
    bracket = type_part.find("[")
    if bracket < 0:
        return Field(name, type_part)
    inner = type_part[bracket + 1:type_part.index("]", bracket)]
    return Field(name, type_part[:bracket], True, int(inner) if inner else 0)


def parse_schema(text: str) -> Schema:
    """Parse the schema text stored in the MCAP schema record / Schema.msg."""
    schema = Schema()
    lines = iter(text.splitlines())
    target = schema.fields
    for raw in lines:
        line = raw.strip()
        if not line:
            continue
        if line.startswith("### version:"):
            if int(line.split(":", 1)[1]) != SCHEMA_VERSION:
                raise ValueError(f"unsupported schema version in {line!r}")
        elif line.startswith("### hash:"):
            value = line.split(":", 1)[1].strip()
            schema.hash = int(value) if value.isdigit() else 0  # implementation-defined, see spec
        elif line.startswith("### channel_name:"):
            schema.channel_name = line.split(":", 1)[1].strip()
        elif line.startswith("====="):
            header = next(lines).strip()
            if not header.startswith("MSG: "):
                raise ValueError(f"expected 'MSG: <type>' after separator, got {header!r}")
            type_name = header[5:].strip()
            nxt = next(lines).strip()
            if nxt.startswith("ENCODING: "):
                body = "\n".join(l for l in lines)  # rest of the text belongs to it
                schema.custom_schemas[type_name] = (nxt[10:].strip(), body)
                break
            target = schema.custom_types.setdefault(type_name, [])
            if nxt:
                target.append(_parse_field_line(nxt))
        else:
            target.append(_parse_field_line(line))
    return schema


def get_bit(mask: bytes, index: int) -> bool:
    return bool(mask[index >> 3] & (1 << (index & 7)))


class _Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def number(self, type_name: str):
        fmt, size = _STRUCT[type_name]
        if self.pos + size > len(self.data):
            raise ValueError("payload truncated")
        (value,) = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += size
        return value.decode("latin-1") if type_name == "char" else value

    def u32(self) -> int:
        return self.number("uint32")


def _parse_field(f: Field, schema: Schema, reader: _Reader, prefix: str, out: dict) -> None:
    name = f.name if not prefix else f"{prefix}/{f.name}"
    count = f.array_size
    if f.is_vector and f.array_size == 0:
        count = reader.u32()

    def one(var_name: str) -> None:
        if f.is_basic:
            out[var_name] = reader.number(f.type_name)
        elif f.type_name in schema.custom_types:
            for sub in schema.custom_types[f.type_name]:
                _parse_field(sub, schema, reader, var_name, out)
        else:
            raise ValueError(f"type {f.type_name!r} has an opaque encoding; cannot continue")

    if not f.is_vector:
        one(name)
    else:
        for i in range(count):
            one(f"{name}[{i}]")


def parse_snapshot(schema: Schema, active_mask: bytes, payload: bytes) -> Dict[str, object]:
    """Decode one snapshot into {"field/path[i]": value}. Disabled fields are absent."""
    out: Dict[str, object] = {}
    reader = _Reader(payload)
    for index, f in enumerate(schema.fields):
        if get_bit(active_mask, index):
            _parse_field(f, schema, reader, "", out)
    if reader.pos != len(payload):
        raise ValueError(f"{len(payload) - reader.pos} trailing bytes in payload")
    return out


def split_mcap_message(data: bytes) -> Tuple[bytes, bytes]:
    """Split an MCAPSink message body into (active_mask, payload)."""
    (mask_len,) = struct.unpack_from("<I", data, 0)
    mask = data[4:4 + mask_len]
    (payload_len,) = struct.unpack_from("<I", data, 4 + mask_len)
    start = 8 + mask_len
    payload = data[start:start + payload_len]
    if start + payload_len != len(data):
        raise ValueError("MCAP message body has trailing bytes")
    return mask, payload
