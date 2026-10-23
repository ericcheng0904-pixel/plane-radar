#!/usr/bin/env python3
"""Fail if a function of the firmware's own code has a stack frame over its limit.

The web pages, the screens and the Wi-Fi setup all run on Arduino's loopTask,
which has 8 KB of stack (CONFIG_ARDUINO_LOOP_STACK_SIZE) for everything nested
under loop(): WiFiManager, the web server, a page handler, printf, lwIP and any
interrupt that arrives meanwhile. v2.0.13's settings page overflowed it: the
compiler inlined four section senders into one frame of 3,568 bytes, the About
section's 2,480 went under it and printf's 1,072 under that. Nothing warned;
the radar rebooted ("A stack overflow in task loopTask").

GCC's -fstack-usage writes every function's frame size to a .su file beside
its object file. This script builds the firmware with it, into a build
directory of its own (.pio/stack-usage, so the regular .pio/build is left
alone), and holds every function of src/ and include/ to a limit:

  * 1024 bytes for any function: an eighth of loopTask's stack.
  * 512 bytes in src/services/portal_pages.cpp. Its functions nest three or
    four deep under the web server (handler, section, helper, putf); its page
    buffer is what keeps them small, and this keeps it that way.
  * A dynamic frame (a variable-length array, alloca) fails anywhere: its size
    is not known.

A std::function invoker (written in a library header) counts as the code it
wraps: GCC inlines a route's whole handler into it. Every line of a .su file
is checked, not one per name: GCC writes all of a file's route invokers under
one name (without the lambda's number), and a function and its clones share
one too.

Allowed above the limit, each with a cap of its own so it cannot grow
unnoticed: the three network calls of the fetch task, which has a 12 KB stack
of its own (config::kAdsbTaskStackBytes) and nothing of the web server's
under it.

What overflows is a chain of frames, not one; GCC 8.4 cannot write the call
graph (-fcallgraph-info), so a limit per frame is what keeps every chain short.
With these limits the settings page needs about 3 KB of loopTask's 8 KB, and
the deepest request (/screen.bmp, which draws a frame) under 4 KB.

    python3 scripts/check_stack_usage.py             build, then check
    python3 scripts/check_stack_usage.py --no-build  check the last such build
    PIO=/path/to/pio python3 scripts/check_stack_usage.py

CI runs it on every push (.github/workflows/quality.yml), after its own tests
(scripts/test_check_stack_usage.py).

Exit status: 0 within limits, 1 over a limit, 2 nothing to check.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ENV = "supermini"
DEFAULT_BUILD_DIR = ROOT / ".pio" / "stack-usage"

LIMIT = 1024
# Source file under src/ -> limit.
FILE_LIMITS = {
    "services/portal_pages.cpp": 512,
}
# Fetch task only: (file, start of the function's name as GCC writes it) -> cap.
ALLOWED = {
    # Town search: its URL buffers and JSON documents.
    ("geocode.cpp", "services::geocode::{anonymous}::Outcome services::geocode::"
                    "{anonymous}::search("): 1536,
    # The update check, with the steps the compiler inlined into it.
    ("ota.cpp", "void services::ota::{anonymous}::check("): 1536,
    # One aircraft fetch, with the steps the compiler inlined into it.
    ("adsb_client.cpp", "services::adsb::FetchResult services::adsb::fetchUpdate("): 1536,
}
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}


def own_files() -> set[str]:
    """Basenames of the project's sources and headers (GCC writes basenames)."""
    names = set()
    for top in ("src", "include"):
        for path in (ROOT / top).rglob("*"):
            if path.suffix in SOURCE_SUFFIXES:
                names.add(path.name)
    return names


def build(env: str, build_dir: Path) -> None:
    environ = dict(os.environ)
    environ["PLATFORMIO_BUILD_DIR"] = str(build_dir)
    environ["PLATFORMIO_BUILD_FLAGS"] = (
        environ.get("PLATFORMIO_BUILD_FLAGS", "") + " -fstack-usage").strip()
    pio = environ.get("PIO", "pio")
    print(f"Building {env} with -fstack-usage into {build_dir} ...", flush=True)
    subprocess.run([pio, "run", "-e", env], cwd=ROOT, env=environ, check=True)


class Frame(NamedTuple):
    unit: str  # the source file compiled, under src/ (services/portal_pages.cpp)
    file: str  # where the function is written (portal_pages.cpp, setup_text.h)
    line: int
    function: str
    qualifier: str  # static, dynamic or dynamic,bounded
    size: int
    su_line: int  # its line in the unit's .su file: names are not unique


def read_frames(su_root: Path) -> list[Frame]:
    """Every frame of the project's own functions, largest first."""
    ours = own_files()
    frames = []
    for su in sorted(su_root.rglob("*.su")):
        # src/services/portal_pages.cpp.su <- src/services/portal_pages.cpp;
        # a .su left behind by a deleted source file is not read.
        unit = su.relative_to(su_root).with_suffix("")
        source = ROOT / "src" / unit
        if not source.exists():
            continue
        lines = su.read_text(errors="replace").splitlines()
        for su_line, line in enumerate(lines, start=1):
            parts = line.rsplit("\t", 2)
            if len(parts) != 3:
                continue
            where, size, qualifier = parts
            fields = where.split(":", 3)
            if len(fields) != 4 or not size.isdigit():
                continue
            file = os.path.basename(fields[0])
            function = fields[3]
            # A std::function's invoker is written in a library header, but
            # the callable it wraps is ours, and GCC inlines a route's whole
            # handler into it (v2.0.12's /settings lambda held the page).
            # Any other library function instantiated here is not ours to fix.
            if file not in ours and not (file == "std_function.h" and "_Functor" in function):
                continue
            if function.startswith(")"):
                # GCC cuts a name after its last '.', which for a variadic
                # function is the "..." of its parameters.
                function = "(variadic, name cut by GCC) " + function
            # Every line, even one whose name and line repeat an earlier one's:
            # each route's invoker is a frame of its own.
            frames.append(Frame(unit.as_posix(), file, int(fields[1]), function,
                                qualifier, int(size), su_line))
    return sorted(frames, key=lambda f: -f.size)


def limit_for(frame: Frame) -> tuple[int, bool]:
    for (allowed_file, prefix), cap in ALLOWED.items():
        if frame.file == allowed_file and frame.function.startswith(prefix):
            return cap, True
    return FILE_LIMITS.get(frame.unit, LIMIT), False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--no-build", action="store_true",
                        help="check the .su files of the last build instead of building")
    parser.add_argument("--env", default=DEFAULT_ENV)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR,
                        help="PlatformIO build_dir for the -fstack-usage build "
                             "(default: .pio/stack-usage)")
    parser.add_argument("--top", type=int, default=12, help="largest frames to list")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    if not args.no_build:
        try:
            build(args.env, build_dir)
        except (OSError, subprocess.CalledProcessError) as err:
            print(f"error: the build failed: {err}", file=sys.stderr)
            return 2
    su_root = build_dir / args.env / "src"
    frames = read_frames(su_root) if su_root.is_dir() else []
    if not frames:
        print(f"error: no stack usage of the project's code under {su_root}; "
              "run without --no-build", file=sys.stderr)
        return 2

    failures = []
    allowed = []
    for frame in frames:
        limit, listed = limit_for(frame)
        # The .su line tells apart frames of one name (route invokers, clones)
        # and names the unit of a function written in a header.
        where = (f"{frame.file}:{frame.line} {frame.function} "
                 f"[{frame.unit}.su line {frame.su_line}]")
        if frame.qualifier.startswith("dynamic"):
            failures.append(f"{where}: {frame.size} bytes and more ({frame.qualifier}): "
                            "a variable-length array or alloca; give it a fixed size")
        elif frame.size > limit:
            failures.append(f"{where}: {frame.size} bytes, limit {limit}")
        elif listed:
            allowed.append(f"{frame.size:5d} of {limit:5d}  {where}")

    print(f"\nLargest stack frames of src/ and include/ ({len(frames)} frames):")
    for frame in frames[:args.top]:
        limit, _ = limit_for(frame)
        print(f"  {frame.size:5d} (limit {limit:4d})  {frame.file}:{frame.line} "
              f"{frame.function[:110]}")
    if allowed:
        print("\nAllowed above the limit (fetch task, 12 KB stack):")
        for entry in allowed:
            print(f"  {entry}")
    if failures:
        print("\nOver the limit. loopTask has 8 KB for loop(), the web server, a page and "
              "printf together;\nmove big buffers off the stack (portal pages: put/putf "
              "write into the page buffer),\nor split the function with "
              "__attribute__((noinline)) so the frames do not add up:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(f"\nAll within limits: {LIMIT} bytes, "
          + ", ".join(f"{n} in {f}" for f, n in FILE_LIMITS.items()) + ".")
    return 0


if __name__ == "__main__":
    sys.exit(main())
