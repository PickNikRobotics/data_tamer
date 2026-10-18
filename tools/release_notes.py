#!/usr/bin/env python3
"""Print the notes of one release: its section of data_tamer_cpp/CHANGELOG.rst.

    python3 tools/release_notes.py --version 2.0.0 > release-notes.md

The section runs from the line after its title ("2.0.0 (2026-11-01)" and its
underline) to the next title. The RST is converted to Markdown only as far as
the changelog uses it: ``code`` becomes `code` and `text <url>`_ a link. Exit
status 1, with a message, when no section is titled with that version; the
"Unreleased" head is never a release. Used by .github/workflows/release.yml.
"""
import argparse
import pathlib
import re
import sys

CHANGELOG = pathlib.Path(__file__).resolve().parent.parent / "data_tamer_cpp" / "CHANGELOG.rst"
# A title is a line underlined with dashes ("^^^^" frames the document title).
TITLE = re.compile(r"^(\S[^\n]*)\n-{3,}[ \t]*$", re.M)


def to_markdown(rst):
    rst = re.sub(r"(?<!`)`([^`<]+?)\s+<([^>]+)>`_", r"[\1](\2)", rst)
    return re.sub(r"``([^`]+)``", r"`\1`", rst)


def extract(text, version):
    titles = list(TITLE.finditer(text))
    for i, title in enumerate(titles):
        if title.group(1).split()[0] == version:
            end = titles[i + 1].start() if i + 1 < len(titles) else len(text)
            return to_markdown(text[title.end():end].strip()) + "\n"
    found = ", ".join(t.group(1).split()[0] for t in titles)
    sys.exit(f"error: {CHANGELOG.name} has no section titled {version} (sections: {found})")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--version", required=True, help="X.Y.Z")
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        parser.error(f"{args.version!r} is not a release number X.Y.Z")
    sys.stdout.write(extract(CHANGELOG.read_text(encoding="utf-8"), args.version))


if __name__ == "__main__":
    main()
