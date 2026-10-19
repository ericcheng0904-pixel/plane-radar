"""
PlatformIO pre: script. Defines PLANE_RADAR_VERSION (include/config.h) from
`git describe`, so the settings page, the User-Agent and the over-the-air
updater all see the tag the image was built from:

    v2.1.0                 built at the release tag
    v2.1.0-3-g1a2b3c4      three commits after it
    v2.1.0-dirty           with uncommitted changes
    1a2b3c4                no v2+ tag in the history (a clone without tags)
    dev                    no git at all (a source archive)

Only tags from v2 on count. The upstream project's v1.* tags are in this
history too, and "v1.1.4-39-g1a2b3c4" would name a release of firmware this is
not. Only the first form is a release (util::release_version); the updater
leaves every other build alone.

PLANE_RADAR_VERSION in the environment overrides git, for reproducing a build.

The macro is on the command line of every compile: a new version changes the
command line, so SCons recompiles everything (~20 s) without a clean.
"""

Import("env")  # noqa: F821 -- provided by SCons

import os
import re
import subprocess

TAG_GLOB = "v[2-9].*"
# The version ends up in a C string literal and an HTTP header: no quotes,
# spaces or control characters, and short enough for the User-Agent.
_SAFE_VERSION = re.compile(r"[0-9A-Za-z][0-9A-Za-z._+-]{0,47}")


def git_describe(project_dir):
    try:
        result = subprocess.run(
            ["git", "describe", "--tags", "--dirty", "--always", "--match", TAG_GLOB],
            cwd=project_dir, capture_output=True, text=True, timeout=20, check=True)
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip() or None


def build_version(project_dir):
    version = os.environ.get("PLANE_RADAR_VERSION", "").strip() or git_describe(project_dir) or "dev"
    if not _SAFE_VERSION.fullmatch(version):
        print("version.py: ignoring unusable version %r, using \"dev\"" % version)
        version = "dev"
    return version


VERSION = build_version(env.subst("$PROJECT_DIR"))  # noqa: F821
env.Append(CPPDEFINES=[("PLANE_RADAR_VERSION", env.StringifyMacro(VERSION))])  # noqa: F821
print("Plane Radar version: %s" % VERSION)
