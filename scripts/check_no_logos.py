#!/usr/bin/env python3
"""Fail if airline logo data, or any file over 1 MB, is tracked in git.

Airline logos are the airlines' trademarks. They live in a private logo pack
(scripts/build_airline_brands.py --logos writes the git-ignored
.cache/logo-pack.bin), flashed once over USB into the radar's own "logos"
partition, and must never reach the repository, and with it a release, OTA or
web-flasher image. .gitignore keeps the pack out; this is the backstop for a
forced add, a renamed copy or data pasted by hand.
CI runs it on every push (.github/workflows/quality.yml). Locally:

    python3 scripts/check_no_logos.py            every file in the index
    python3 scripts/check_no_logos.py FILE...    just these files

It fails on
  * arrays named the way the generator names logos: "k", an ICAO code, a pixel
    size, "[";
  * image data under any name: 1024 or more four-digit hex literals in one
    file, which is a single 32x32 RGB565 image;
  * the private logo pack (logo-pack.bin), a *.local.h logo file or the
    generator's cache (.cache/) being tracked;
  * any file over 1 MB. The logo table was the only thing this repository ever
    held of that size (1.75 MB); the largest legitimate file is under 200 KB.

Exit status: 0 clean, 1 something found.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAX_BYTES = 1024 * 1024
LOGO_ARRAY = re.compile(rb"\bk[A-Z]{3}(?:16|24|32|48|64)\s*\[")
HEX16 = re.compile(rb"\b0x[0-9A-Fa-f]{4}\b")
MAX_HEX16 = 32 * 32
PRIVATE_PATH = re.compile(r"(?:^|/)(?:logo-pack\.bin$|[^/]*logo[^/]*\.local\.h$|\.cache/)",
                          re.IGNORECASE)


def index_entries() -> list[tuple[str, str]]:
    """(path, blob id) for every file in the index, submodules skipped."""
    out = subprocess.run(["git", "ls-files", "-s", "-z"], cwd=ROOT, check=True,
                         capture_output=True).stdout
    entries = []
    for record in out.split(b"\0"):
        if not record:
            continue
        meta, path = record.split(b"\t", 1)
        mode, blob, _stage = meta.split()
        if mode != b"160000":
            entries.append((path.decode("utf-8", "replace"), blob.decode()))
    return entries


def blob_contents(blobs: list[str]) -> list[bytes]:
    """Contents of the given blobs, in order, read in one git process."""
    if not blobs:
        return []
    data = subprocess.run(["git", "cat-file", "--batch"], cwd=ROOT, check=True,
                          capture_output=True,
                          input=("\n".join(blobs) + "\n").encode()).stdout
    contents = []
    pos = 0
    for _ in blobs:
        end = data.index(b"\n", pos)
        size = int(data[pos:end].split()[2])
        contents.append(data[end + 1:end + 1 + size])
        pos = end + 1 + size + 1
    return contents


# Signed release images are big by nature; make_release.py and pages.yml
# check those for logo data themselves.
RELEASE_IMAGE = re.compile(r"^site/(ota|flash)/[^/]+\.bin$")


def problems(path: str, content: bytes) -> list[str]:
    found = []
    if len(content) > MAX_BYTES and not RELEASE_IMAGE.match(path):
        found.append(f"{len(content):,} bytes, over the 1 MB limit")
    if PRIVATE_PATH.search(path):
        found.append("the private logo pack, a local logo file or the logo cache is tracked")
    match = LOGO_ARRAY.search(content)
    if match:
        line = content.count(b"\n", 0, match.start()) + 1
        found.append(f"line {line}: logo array {match.group().decode()}")
    hex16 = len(HEX16.findall(content))
    if hex16 >= MAX_HEX16:
        found.append(f"{hex16} four-digit hex literals: looks like RGB565 image data")
    return found


def main(argv: list[str]) -> int:
    if argv:
        files = [(p, Path(p).read_bytes()) for p in argv]
    else:
        entries = index_entries()
        files = list(zip([p for p, _ in entries], blob_contents([b for _, b in entries])))
    failures = 0
    for path, content in files:
        for problem in problems(path, content):
            print(f"{path}: {problem}")
            failures += 1
    if failures:
        print("\nAirline logos are the airlines' trademarks and must not be committed. "
              "Keep them in the git-ignored logo pack, .cache/logo-pack.bin "
              "(scripts/build_airline_brands.py --logos), flashed to the radar over USB, "
              "and unstage the files above.")
        return 1
    print(f"ok: {len(files)} files, no logo data, none over 1 MB")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
