"""The compiled decoder against the reference walker.

parse_snapshot() and parse_snapshot_values() decode through plans compiled per schema
and active mask; _parse_snapshot_reference() walks the schema value by value and is the
specification. On every input here they give the same values (same names, order and
types, floats bit for bit), or raise the same exception with the same message.
"""
import copy
import pickle
import random
import struct
import threading
import time
import unittest

import data_tamer_parser as dt
from test_data_tamer_parser import SEPARATOR, VECTORS, read

HEADER = "### version: 5\n### hash: 0\n### channel_name: c\n\n"
parse_snapshot = dt.parse_snapshot  # ExistingTests replaces the module's one


def normalized(values: dict) -> list:
    """The items with the type of each value, and floats as their bits (NaN == NaN)."""
    return [(name, type(v).__name__, struct.pack("<d", v) if isinstance(v, float) else v)
            for name, v in values.items()]


def outcome(decode, *args):
    """("returns", normalized items) or ("raises", exception type, message)."""
    try:
        values = decode(*args)
    except Exception as err:  # what must agree is the type and the message
        return ("raises", type(err).__name__, str(err))
    return ("returns", normalized(values))


def values_as_dict(schema, mask, payload) -> dict:
    names, values = dt.parse_snapshot_values(schema, mask, payload)
    assert type(names) is tuple and type(values) is tuple, (type(names), type(values))
    assert len(names) == len(values) == len(set(names)), (names, values)
    return dict(zip(names, values))


def disagreement(schema, mask, payload, reference_schema=None):
    """None if both entry points agree with the reference, twice (compiling, then cached);
    else a description."""
    expected = outcome(dt._parse_snapshot_reference, reference_schema or schema, mask, payload)
    for attempt in range(2):
        for decode in (parse_snapshot, values_as_dict):
            got = outcome(decode, schema, mask, payload)
            if got != expected:
                return (f"{decode.__name__} (attempt {attempt}) gave {got}, the reference "
                        f"{expected}, for mask {bytes(mask).hex()} payload {bytes(payload).hex()}")
    return None


def mask_of(bits) -> bytes:
    return bytes(sum(1 << j for j, on in enumerate(bits[i:i + 8]) if on)
                 for i in range(0, len(bits), 8))


def encode(rng: random.Random, schema: dt.Schema, f: dt.Field, out: bytearray) -> None:
    """Random bytes in the layout of `f`; a few undecodable bytes for an opaque or
    undefined type."""
    if f.is_vector and not f.array_size:
        count = rng.choice((0, 0, 1, 2, 3, 5))
        out += struct.pack("<I", count)
    else:
        count = f.array_size if f.is_vector else 1
    for _ in range(count):
        if f.is_basic:
            out += rng.randbytes(dt._STRUCT[f.type_name].size)
        elif f.type_name in schema.custom_types:
            for sub in schema.custom_types[f.type_name]:
                encode(rng, schema, sub, out)
        else:
            out += rng.randbytes(rng.randint(0, 4))


def snapshot(rng: random.Random, schema: dt.Schema, enabled: float = 0.7):
    bits = [rng.random() < enabled for _ in schema.fields]
    payload = bytearray()
    for f, on in zip(schema.fields, bits):
        if on:
            encode(rng, schema, f, payload)
    return mask_of(bits), bytes(payload)


def variants(rng: random.Random, mask: bytes, payload: bytes):
    """The snapshot, then malformed ones: cut, too long, a count of 2^32 - 1, short masks."""
    yield mask, payload
    yield mask + b"\xff", payload  # bits past the schema are ignored
    yield mask, payload + bytes([rng.randrange(256)])
    if payload:
        yield mask, payload[:rng.randrange(len(payload))]
        yield mask, payload[:-1]
        at = rng.randrange(len(payload))
        yield mask, payload[:at] + b"\xff\xff\xff\xff" + payload[at + 4:]
    if mask:
        yield mask[:-1], payload
        yield b"", payload


BASIC = list(dt._STRUCT)


def random_field(rng: random.Random, name: str, types: list) -> dt.Field:
    pick = rng.random()
    if pick < 0.03:
        type_name = "Blob"  # opaque
    elif pick < 0.05:
        type_name = "Missing"  # defined nowhere
    elif types and pick < 0.45:
        type_name = rng.choice(types)
    else:
        type_name = rng.choice(BASIC)
    shape = rng.random()
    if shape < 0.5:
        return dt.Field(name, type_name)
    if shape < 0.75:
        return dt.Field(name, type_name, True, rng.randint(1, 3))
    return dt.Field(name, type_name, True, 0)


def random_schema(rng: random.Random) -> dt.Schema:
    """Basic types, fixed and dynamic arrays, custom types nested up to 5 deep, vectors
    inside them, types with no field, opaque and undefined types, and top-level names
    that collide with flattened ones ("a/b", "a[0]")."""
    types = [f"T{i}" for i in range(rng.randint(0, 5))]
    custom = {name: [random_field(rng, sub, types[i + 1:])  # no cycle
                     for sub in rng.sample("abcd", rng.randint(0, 3))]
              for i, name in enumerate(types)}
    top = rng.sample(["a", "b", "c", "a/a", "a/b", "b/a", "a[0]", "a[1]", "x", "y", "z"],
                     rng.randint(1, 10))
    return dt.Schema(channel_name="random", fields=[random_field(rng, n, types) for n in top],
                     custom_types=custom, custom_schemas={"Blob": ("proto", "message Blob {}")})


class RandomizedSchemas(unittest.TestCase):
    def check_random(self, seed: int, schemas: int):
        rng = random.Random(seed)
        decoded = raised = 0
        for _ in range(schemas):
            schema = random_schema(rng)
            reference = copy.deepcopy(schema)
            for _ in range(4):
                for mask, payload in variants(rng, *snapshot(rng, schema)):
                    problem = disagreement(schema, mask, payload, reference)
                    self.assertIsNone(problem, f"{problem}\nschema: {schema}")
                    try:
                        dt._parse_snapshot_reference(reference, mask, payload)
                        decoded += 1
                    except ValueError:
                        raised += 1
        self.assertGreater(min(decoded, raised), schemas)  # both outcomes are exercised

    def test_random_schemas_and_snapshots(self):
        self.check_random(seed=20261018, schemas=600)

    def test_random_schemas_with_a_tiny_plan_budget(self):
        # most top-level fields are too large to compile and go to the reference walker
        budget = dt._PLAN_BUDGET
        dt._PLAN_BUDGET = 6
        try:
            self.check_random(seed=7, schemas=200)
        finally:
            dt._PLAN_BUDGET = budget


class GoldenVectors(unittest.TestCase):
    def check(self, schema, mask: bytes, payload: bytes):
        self.assertIsNone(disagreement(schema, mask, payload))
        for end in range(len(payload)):
            self.assertIsNone(disagreement(schema, mask, payload[:end]))

    def test_golden_snapshots_every_cut_and_random_masks(self):
        for text in ("schema.txt", "schema.yaml"):
            schema = dt.parse_schema(read(text).decode())
            for stem in ("snapshot_full", "snapshot_masked"):
                with self.subTest(text=text, stem=stem):
                    self.check(schema, read(stem + ".mask"), read(stem + ".payload"))
            rng = random.Random(4)
            for _ in range(200):
                for mask, payload in variants(rng, *snapshot(rng, schema, enabled=0.5)):
                    self.assertIsNone(disagreement(schema, mask, payload))

    def test_nested_schema(self):
        schema = dt.parse_schema(read("schema_nested.yaml").decode())
        rng = random.Random(5)
        for _ in range(100):
            for mask, payload in variants(rng, *snapshot(rng, schema, enabled=0.5)):
                self.assertIsNone(disagreement(schema, mask, payload))

    def test_recording_104(self):
        try:
            from mcap.reader import make_reader
        except ImportError:
            self.skipTest("mcap not installed (pip install mcap)")
        schemas, count = {}, 0
        with open(VECTORS / "v4" / "recorded_1.0.4.mcap", "rb") as stream:
            for mcap_schema, _, message in make_reader(stream).iter_messages():
                if mcap_schema.id not in schemas:
                    schemas[mcap_schema.id] = dt.parse_schema(mcap_schema.data.decode())
                self.check(schemas[mcap_schema.id], *dt.split_mcap_message(message.data))
                count += 1
        self.assertGreater(count, 10)


class ExistingTests(unittest.TestCase):
    """The other test modules, run again with every parse_snapshot() call checked against
    the reference: the golden vectors, the 1.0.4 recording, the robustness cases."""

    def test_every_decode_of_the_other_tests_agrees_with_the_reference(self):
        import test_data_tamer_parser
        import test_robustness
        calls, problems = [], []

        def checked(schema, mask, payload):
            calls.append(None)
            problem = disagreement(schema, mask, payload)
            if problem:
                problems.append(problem)
            return parse_snapshot(schema, mask, payload)

        suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromModule(module)
                                   for module in (test_data_tamer_parser, test_robustness))
        result = unittest.TestResult()
        dt.parse_snapshot = checked
        try:
            suite.run(result)
        finally:
            dt.parse_snapshot = parse_snapshot
        self.assertEqual(problems, [])
        self.assertEqual([str(test) for test, _ in result.failures + result.errors], [])
        self.assertGreater(len(calls), 40)


class EdgeCases(unittest.TestCase):
    def assert_agrees(self, text: str, mask: bytes, payload: bytes):
        problem = disagreement(dt.parse_schema(HEADER + text), mask, payload)
        self.assertIsNone(problem)

    def test_huge_count_is_rejected_before_anything_is_built(self):
        schema = dt.parse_schema(HEADER + f"float64[] v\nP[] p\n{SEPARATOR}\nMSG: P\n"
                                 "float64 x\nuint32 n\n")
        for mask in (b"\x01", b"\x02"):
            with self.assertRaisesRegex(ValueError, "^payload truncated$"):
                dt.parse_snapshot(schema, mask, b"\xff\xff\xff\xff" + bytes(16))
            self.assertEqual(schema._plans[mask].ops[0][1].structs, {})
            self.assertEqual(schema._plans[mask].ops[0][1].names, ())

    def test_exponential_cyclic_type_compiles_in_bounded_time(self):
        # A holds two A: walking every path would never end; the first too deep one ends it
        for top in ("A[] as", "A a", "A[2] a"):
            schema = dt.parse_schema(HEADER + f"float64 x\n{top}\n{SEPARATOR}\nMSG: A\nA l\nA r\n")
            start = time.perf_counter()
            for payload in (b"", bytes(8), bytes(12) + b"\x01"):
                self.assertIsNone(disagreement(schema, b"\x03", payload))
            self.assertLess(time.perf_counter() - start, 0.25)

    def test_depth_limit(self):
        for depth in (dt.MAX_SCHEMA_DEPTH, dt.MAX_SCHEMA_DEPTH + 1):
            leaf = f"{SEPARATOR}\nMSG: T{depth}\nfloat32[] x\n"
            text = (f"uint8 before\nT1 t\nT1[] ts\n"
                    + "".join(f"{SEPARATOR}\nMSG: T{i}\nT{i + 1} t\n" for i in range(1, depth))
                    + leaf)
            for mask in (b"\x03", b"\x05"):
                for payload in (b"\x07", b"\x07" + bytes(4), b"\x07\x01\0\0\0" + bytes(4)):
                    with self.subTest(depth=depth, mask=mask, payload=payload):
                        self.assert_agrees(text, mask, payload)

    def test_strings_and_chars(self):
        text = (f"char c\nchar[3] cs\nchar[] s\nNamed[] ns\n{SEPARATOR}\nMSG: Named\n"
                "char k\nuint8 v\n")
        payload = b"abc" + b"d\x03\0\0\0\xe9f\0" + b"\x02\0\0\0" + b"x\x01y\x02"
        self.assert_agrees(text, b"\x0f", payload)
        values = dt.parse_snapshot(dt.parse_schema(HEADER + text), b"\x0f", payload)
        self.assertEqual(values["s[0]"], "\u00e9")
        self.assertEqual(values["ns[1]/k"], "y")

    def test_plan_too_large_for_the_budget_is_walked(self):
        text = "float64[300] a\nfloat64 b\nfloat64[] c\n"
        budget = dt._PLAN_BUDGET
        dt._PLAN_BUDGET = 100
        try:
            payload = bytes(2408) + struct.pack("<I", 2) + bytes(16)
            schema = dt.parse_schema(HEADER + text)
            self.assertIsNone(disagreement(schema, b"\x07", payload))
            self.assertEqual([op[0] for op in schema._plans[b"\x07"].ops],
                             [dt._WALK, dt._WALK, dt._WALK])
        finally:
            dt._PLAN_BUDGET = budget


class Columns(unittest.TestCase):
    TEXT = HEADER + f"float64 x\nfloat32[] v\nP p\n{SEPARATOR}\nMSG: P\nint8 a\nchar[] s\n"

    @staticmethod
    def payload(v: int, s: int) -> bytes:
        return (bytes(8) + struct.pack("<I", v) + bytes(4 * v) + b"\x01"
                + struct.pack("<I", s) + b"z" * s)

    def test_names_are_one_object_per_layout(self):
        schema = dt.parse_schema(self.TEXT)
        names, values = dt.parse_snapshot_values(schema, b"\x07", self.payload(2, 1))
        self.assertEqual(names, ("x", "v[0]", "v[1]", "p/a", "p/s[0]"))
        self.assertEqual(values, (0.0, 0.0, 0.0, 1, "z"))
        again, _ = dt.parse_snapshot_values(schema, b"\x07", bytearray(self.payload(2, 1)))
        self.assertIs(again, names)
        longer, _ = dt.parse_snapshot_values(schema, b"\x07", self.payload(3, 1))
        self.assertEqual(len(longer), 6)
        self.assertIs(dt.parse_snapshot_values(schema, b"\x07", self.payload(2, 1))[0], names)
        self.assertEqual(dt.parse_snapshot_values(schema, b"\x04", b"\x01\0\0\0\0"),
                         (("p/a",), (1,)))
        self.assertEqual(dt.parse_snapshot_values(schema, b"\0", b""), ((), ()))

    def test_fixed_layout_names_are_the_plan_names(self):
        schema = dt.parse_schema(read("schema.txt").decode())
        mask, payload = read("snapshot_full.mask"), read("snapshot_full.payload")
        first = dt.parse_snapshot_values(schema, mask, payload)
        self.assertEqual(dict(zip(*first)), dt.parse_snapshot(schema, mask, payload))
        self.assertIs(dt.parse_snapshot_values(schema, mask, payload)[0], first[0])

    def test_repeated_name_keeps_its_last_value_at_its_first_place(self):
        schema = dt.parse_schema(HEADER + f"float64 a/b\nA a\nint8 c\n{SEPARATOR}\nMSG: A\n"
                                 "int8 b\n")
        payload = struct.pack("<d", 0.5) + b"\x07\x09"
        self.assertEqual(dt.parse_snapshot(schema, b"\x07", payload), {"a/b": 7, "c": 9})
        self.assertEqual(dt.parse_snapshot_values(schema, b"\x07", payload),
                         (("a/b", "c"), (7, 9)))

    def test_layout_cache_is_bounded(self):
        limit = dt._MAX_LAYOUT_NAMES
        dt._MAX_LAYOUT_NAMES = 20
        try:
            schema = dt.parse_schema(self.TEXT)
            for v in (1, 2, 30, 3, 1, 2):
                names, values = dt.parse_snapshot_values(schema, b"\x07", self.payload(v, 0))
                self.assertEqual(len(names), v + 2)
                self.assertLessEqual(schema._plans[b"\x07"].layout_names, 20)
        finally:
            dt._MAX_LAYOUT_NAMES = limit


class Caches(unittest.TestCase):
    def test_plans_per_schema_are_bounded(self):
        schema = dt.parse_schema(HEADER + "".join(f"uint8 v{i}\n" for i in range(6)))
        for mask in range(64):
            payload = bytes(range(bin(mask).count("1")))
            expected = {f"v{i}": n for n, i in enumerate(i for i in range(6) if mask >> i & 1)}
            self.assertEqual(dt.parse_snapshot(schema, bytes([mask]), payload), expected)
            self.assertLessEqual(len(schema._plans), dt._MAX_PLANS)

    def test_structs_and_names_per_vector_are_bounded(self):
        schema = dt.parse_schema(HEADER + "uint16[] v\n")
        limit = dt._MAX_VECTOR_NAMES
        dt._MAX_VECTOR_NAMES = 50
        try:
            for count in range(100):
                payload = struct.pack("<I", count) + bytes(2 * count)
                self.assertEqual(dt.parse_snapshot(schema, b"\x01", payload),
                                 {f"v[{i}]": 0 for i in range(count)})
                vector = schema._plans[b"\x01"].ops[0][1]
                self.assertLessEqual(len(vector.structs), dt._MAX_VECTOR_STRUCTS)
                self.assertEqual(len(vector.names), min(count, 50))
        finally:
            dt._MAX_VECTOR_NAMES = limit

    def test_schema_copies_and_pickles_after_decoding(self):
        schema = dt.parse_schema(read("schema.txt").decode())
        mask, payload = read("snapshot_full.mask"), read("snapshot_full.payload")
        expected = dt.parse_snapshot(schema, mask, payload)
        for clone in (copy.copy(schema), copy.deepcopy(schema),
                      pickle.loads(pickle.dumps(schema))):
            self.assertEqual(clone, schema)
            self.assertEqual(dt.parse_snapshot(clone, mask, payload), expected)
        self.assertEqual(len(copy.deepcopy(schema)._plans), 0)

    def test_threads_share_one_schema(self):
        # more masks than plans fit, so threads also clear the cache under each other
        rng = random.Random(9)
        text = read("schema.txt").decode()
        reference = dt.parse_schema(text)
        cases = []
        for _ in range(dt._MAX_PLANS + 8):
            mask, payload = snapshot(rng, reference, enabled=0.5)
            cases.append((mask, payload,
                          normalized(dt._parse_snapshot_reference(reference, mask, payload))))
        schema = dt.parse_schema(text)
        barrier, problems = threading.Barrier(8), []

        def work(seed: int):
            order = random.Random(seed)
            barrier.wait()
            for _ in range(30):
                mask, payload, expected = order.choice(cases)
                if normalized(dt.parse_snapshot(schema, mask, payload)) != expected:
                    problems.append(mask)
                names, values = dt.parse_snapshot_values(schema, mask, payload)
                if normalized(dict(zip(names, values))) != expected:
                    problems.append(mask)

        threads = [threading.Thread(target=work, args=(seed,)) for seed in range(8)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(problems, [])


if __name__ == "__main__":
    unittest.main()
