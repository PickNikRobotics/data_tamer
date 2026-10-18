#!/usr/bin/env python3
"""Check tools/release_notes.py against the 1.0.4 section of the real changelog."""
import pathlib
import subprocess
import sys
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "release_notes.py"


def notes(version):
    return subprocess.run([sys.executable, str(SCRIPT), "--version", version],
                          capture_output=True, text=True)


class ReleaseNotes(unittest.TestCase):
    def test_section_is_cut_between_two_titles_and_converted(self):
        result = notes("1.0.4")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertTrue(lines[0].startswith(
            "* Merge pull request [#68](https://github.com/PickNikRobotics/data_tamer/issues/68)"))
        self.assertTrue(lines[-1].startswith("* Contributors:"))
        self.assertNotIn("1.0.3", result.stdout)
        self.assertNotIn("``", result.stdout)
        self.assertNotIn("`_", result.stdout)

    def test_unknown_version_is_a_clean_error(self):
        result = notes("999.0.0")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("no section titled 999.0.0", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
