"""Time the decoders on schemas built here, standard library only:

    python3 python/benchmark_decode.py [seconds per sample]

For each schema: the reference walker (_parse_snapshot_reference()), parse_snapshot()
and parse_snapshot_values(), in microseconds per snapshot (the median of 5 samples) and
as a speedup over the reference. The results are checked against the reference first.
"""
import random
import statistics
import struct
import sys
import time

import data_tamer_parser as dt

SEPARATOR = "=" * 59
POSE = (f"{SEPARATOR}\nMSG: Point3D\nfloat64 x\nfloat64 y\nfloat64 z\n"
        f"{SEPARATOR}\nMSG: Pose\nPoint3D position\nQuaternion rotation\n"
        f"{SEPARATOR}\nMSG: Quaternion\nfloat64 w\nfloat64 x\nfloat64 y\nfloat64 z\n")
BASIC = "".join(f"{t} {t}_value\n" for t in dt._STRUCT)
# name: (fields and types, length of every dynamic vector)
SCENARIOS = {
    "arm6": ("float64[2] positions\nfloat64[2] velocities\nfloat64[2] efforts\n", 0),
    "scalars125": ("".join(f"float64 v_{i}\n" for i in range(125)), 0),
    "scalars1000": ("".join(f"float64 v_{i}\n" for i in range(1000)), 0),
    "vector1000": ("float64[] samples\n", 1000),
    "poses100": ("Pose[100] poses\n" + POSE, 0),
    "mixed": (BASIC + "float32[] samples\nPose pose\n" + POSE, 10),
    "points100": (f"Point3D[] points\n{SEPARATOR}\nMSG: Point3D\n"
                  "float64 x\nfloat64 y\nfloat64 z\n", 100),
    # elements that hold a vector: decoded by the reference walker
    "joints12": (f"Joint[] joints\n{SEPARATOR}\nMSG: Joint\nchar[] name\nfloat64 q\n", 12),
}


def encode(rng: random.Random, schema: dt.Schema, f: dt.Field, length: int, out: bytearray):
    if f.is_vector and not f.array_size:
        out += struct.pack("<I", length)
        count = length
    else:
        count = f.array_size if f.is_vector else 1
    for _ in range(count):
        if f.type_name in ("float32", "float64"):
            out += dt._STRUCT[f.type_name].pack(rng.uniform(-1e3, 1e3))
        elif f.is_basic:
            out += rng.randbytes(dt._STRUCT[f.type_name].size)
        else:
            for sub in schema.custom_types[f.type_name]:
                encode(rng, schema, sub, length, out)


def per_snapshot(decode, snapshots, seconds: float) -> float:
    """Median microseconds per snapshot over 5 samples."""
    samples = []
    for _ in range(5):
        start, count = time.perf_counter(), 0
        while time.perf_counter() - start < seconds:
            for mask, payload in snapshots:
                decode(mask, payload)
            count += len(snapshots)
        samples.append((time.perf_counter() - start) / count * 1e6)
    return statistics.median(samples)


def main(seconds: float):
    rng = random.Random(1)
    print(f"Python {sys.version.split()[0]}, microseconds per snapshot")
    print(f"{'schema':12} {'values':>6} {'reference':>10} {'parse_snapshot':>20}"
          f" {'parse_snapshot_values':>22}")
    for name, (fields, length) in SCENARIOS.items():
        schema = dt.parse_schema(f"### version: 5\n### hash: 0\n### channel_name: {name}\n\n"
                                 + fields)
        mask = b"\xff" * ((len(schema.fields) + 7) // 8)
        snapshots = []
        for _ in range(50):
            payload = bytearray()
            for f in schema.fields:
                encode(rng, schema, f, length, payload)
            snapshots.append((mask, bytes(payload)))
        for m, p in snapshots:  # correct first
            expected = dt._parse_snapshot_reference(schema, m, p)
            assert dt.parse_snapshot(schema, m, p) == expected, name
            assert dt.parse_snapshot_values(schema, m, p) == (tuple(expected),
                                                              tuple(expected.values())), name
        reference = per_snapshot(lambda m, p: dt._parse_snapshot_reference(schema, m, p),
                                 snapshots, seconds)
        fast = per_snapshot(lambda m, p: dt.parse_snapshot(schema, m, p), snapshots, seconds)
        columns = per_snapshot(lambda m, p: dt.parse_snapshot_values(schema, m, p),
                               snapshots, seconds)
        print(f"{name:12} {len(expected):6} {reference:10.2f}"
              f" {fast:12.2f} ({reference / fast:4.1f}x)"
              f" {columns:14.2f} ({reference / columns:4.1f}x)")


if __name__ == "__main__":
    main(float(sys.argv[1]) if len(sys.argv) > 1 else 0.2)
