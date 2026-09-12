"""Decodes the golden vectors from docs/wire_format/vectors with the reference decoder."""
import json
import pathlib
import unittest

import data_tamer_parser as dt

VECTORS = pathlib.Path(__file__).resolve().parent.parent / "docs" / "wire_format" / "vectors"


def read(name: str) -> bytes:
    return (VECTORS / name).read_bytes()


class GoldenVectors(unittest.TestCase):
    def setUp(self):
        self.schema = dt.parse_schema(read("schema.txt").decode())
        with open(VECTORS / "expected.json") as f:
            self.expected = json.load(f)

    def test_schema(self):
        self.assertEqual(self.schema.channel_name, "wire_test")
        self.assertEqual([f.name for f in self.schema.fields], self.expected["fields"])
        self.assertEqual(sorted(self.schema.custom_types), ["Point3D", "Pose"])

    def check(self, stem: str, key: str):
        values = dt.parse_snapshot(self.schema, read(stem + ".mask"), read(stem + ".payload"))
        self.assertEqual(values, self.expected[key])
        mask, payload = dt.split_mcap_message(read(stem + ".mcap_message"))
        self.assertEqual((mask, payload), (read(stem + ".mask"), read(stem + ".payload")))

    def test_full_snapshot(self):
        self.check("snapshot_full", "full")

    def test_masked_snapshot(self):
        self.check("snapshot_masked", "masked")
        self.assertNotIn("i16", self.expected["masked"])
        self.assertNotIn("pose/stamp", self.expected["masked"])


if __name__ == "__main__":
    unittest.main()
