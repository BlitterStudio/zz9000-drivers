#!/usr/bin/env python3
"""Convert UTF-8 release text to Amiga Latin-1 before archiving.

Run once on a freshly staged release, never on repository sources.
Unmapped characters fail packaging rather than silently losing text.
"""

import argparse
from pathlib import Path


REPLACEMENTS = str.maketrans({
    "\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"',
    "\u2013": "-", "\u2014": "--", "\u2011": "-", "\u2212": "-",
    "\u2026": "...", "\u2022": "*", "\u2190": "<-", "\u2192": "->",
    "\u2500": "-", "\u2502": "|", "\u251c": "+", "\u2514": "+",
})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("staging", type=Path)
    staging = parser.parse_args().staging
    if not staging.is_dir():
        parser.error(f"not a staging directory: {staging}")

    paths = sorted(path for path in staging.rglob("*") if path.is_file() and (
        path.suffix.lower() in {".md", ".txt", ".guide"}
        or path.name.lower() in {"readme", "install zz9000"}
    ))
    # Validate every document before changing any staged file.
    encoded = []
    for path in paths:
        try:
            text = path.read_bytes().decode("utf-8-sig")
            encoded.append((path, text.translate(REPLACEMENTS).encode("latin-1")))
        except UnicodeError as error:
            parser.error(f"{path}: {error}")
    for path, content in encoded:
        path.write_bytes(content)
    print(f"Encoded {len(encoded)} release text files as Amiga Latin-1")


if __name__ == "__main__":
    main()
