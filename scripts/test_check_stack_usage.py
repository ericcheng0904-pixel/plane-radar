"""Tests of scripts/check_stack_usage.py on .su files written here (no build).

    python3 -m unittest discover -s scripts -p 'test_*.py' -v

CI runs them before the stack check itself (.github/workflows/quality.yml).
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "check_stack_usage.py"

# What GCC writes for each route's std::function invoker in portal_pages.cpp:
# one name for all of them, without the lambda's number, cut at the "..." of
# the parameter pack.
INVOKER = ("std_function.h:295:7:) [with _Functor = services::portal::registerLanRoutes("
           "WebServer&, const services::portal::Hooks&)::<lambda()>; _ArgTypes = {}]")


def su(*frames: tuple[str, int]) -> str:
    """A .su file's text: one line per (where, size)."""
    return "".join(f"{where}\t{size}\tstatic\n" for where, size in frames)


class StackCheckTest(unittest.TestCase):
    def check(self, files: dict[str, str]) -> tuple[int, str]:
        """Runs the check on {source under src/: .su text}: (exit status, output)."""
        with tempfile.TemporaryDirectory() as build_dir:
            for source, text in files.items():
                path = Path(build_dir, "supermini", "src", source + ".su")
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            run = subprocess.run(
                [sys.executable, str(SCRIPT), "--no-build", "--build-dir", build_dir],
                capture_output=True, text=True, check=False)
        return run.returncode, run.stdout + run.stderr

    def test_every_route_invoker_is_checked(self):
        # Same name, file and line: only the last used to be checked.
        status, out = self.check({"services/portal_pages.cpp": su((INVOKER, 900), (INVOKER, 80))})
        self.assertEqual(status, 1, out)
        self.assertIn("900 bytes, limit 512", out)
        self.assertIn("[services/portal_pages.cpp.su line 1]", out)

    def test_invokers_within_the_limit_pass(self):
        status, out = self.check({"services/portal_pages.cpp": su((INVOKER, 176), (INVOKER, 80))})
        self.assertEqual(status, 0, out)

    def test_sources_of_one_basename_are_told_apart(self):
        # src/services/geocode.cpp and src/util/geocode.cpp.
        where = "geocode.cpp:40:6:void f()"
        status, out = self.check({"services/geocode.cpp": su((where, 1100)),
                                  "util/geocode.cpp": su((where, 64))})
        self.assertEqual(status, 1, out)
        self.assertIn("1100 bytes, limit 1024", out)


if __name__ == "__main__":
    unittest.main()
