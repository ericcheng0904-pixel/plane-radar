"""
PlatformIO post: script. Keeps firmware.bin inside its app slot.

An over-the-air update is written to whichever app slot is not running, so an
image has to fit the smallest app partition of its table -- 0x1C0000 (1.75 MB)
in partitions/plane_radar.csv. That is not a soft limit: the bootloader will not
start an image longer than its partition. So the build

  - fails when firmware.bin is larger than the slot, and deletes it so it
    cannot be flashed by mistake;
  - warns above 90 % of the slot, while there is still time to make room.

PlatformIO checks a size of its own ("Checking size"), but it adds up ELF
sections, which come out ~80 KB smaller than firmware.bin: the image pads its
flash-mapped segments to 64 KB boundaries. This script measures the .bin. It
also checks the ELF just before PlatformIO does, so a build that is far too big
fails with the explanation below rather than a bare "program size is greater
than maximum allowed".

No image contains airline logos: they live in the radar's own "logos" partition
(scripts/build_airline_brands.py --logos). One that did, ~1.91 MB, would fit no
slot and fail here too.
"""

import os
import re
import subprocess
import sys

try:
    Import("env")  # noqa: F821 -- provided by SCons
except NameError:  # imported by the host tests (tools/ota), not by PlatformIO
    env = None

WARN_FRACTION = 0.90


def parse_size(text):
    text = text.strip()
    if text.lower().startswith("0x"):
        return int(text, 16)
    if text[-1:].upper() in ("K", "M"):
        return int(text[:-1]) * (1024 if text[-1].upper() == "K" else 1024 * 1024)
    return int(text)


def app_slot_bytes(csv_text):
    """Size of the smallest app partition in a partition table CSV."""
    sizes = []
    for line in csv_text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) >= 5 and cols[1] in ("app", "0"):
            sizes.append(parse_size(cols[4]))
    if not sizes:
        raise ValueError("no app partition in the partition table")
    return min(sizes)


def assess(image_bytes, slot_bytes, warn_fraction=WARN_FRACTION):
    """
    ("ok" | "warn" | "fail", message) for an image of `image_bytes` in a slot
    of `slot_bytes`.
    """
    share = image_bytes / slot_bytes
    summary = "firmware.bin is %s bytes, %.1f %% of the %s-byte app slot (0x%X)" % (
        format(image_bytes, ","), 100.0 * share, format(slot_bytes, ","), slot_bytes)
    if image_bytes > slot_bytes:
        return "fail", "%s: %s bytes too many." % (summary, format(image_bytes - slot_bytes, ","))
    if share > warn_fraction:
        return "warn", "%s. Over %d %%: plan for making room before it stops fitting." % (
            summary, int(round(warn_fraction * 100)))
    return "ok", summary + "."


def explain_too_big(logo_reason):
    lines = [
        "An over-the-air update needs two app slots of this size, and the bootloader",
        "will not start an image larger than its slot.",
    ]
    if logo_reason:
        lines.append("This build %s." % logo_reason)
        lines.append("The logos take ~0.4 MB; logo-free images are ~1.5 MB. Logos belong in the")
        lines.append("logo pack (build_airline_brands.py --logos), never in a firmware image.")
    else:
        lines.append("Logo-free images are ~1.5 MB: look for what grew.")
    return "\n".join("*** " + line for line in lines)


def _logo_reason(project_dir, image=None, elf=None):
    """Why the build contains logos (ota_common.find_logos), or None."""
    sys.dont_write_bytecode = True
    sys.path.insert(0, os.path.join(project_dir, "scripts"))
    try:
        import ota_common
    except ImportError:
        return None
    finally:
        sys.path.pop(0)
    return ota_common.find_logos(image or b"", elf)


def _slot(env):
    with open(env.subst("$PARTITIONS_TABLE_CSV"), encoding="utf-8") as fp:
        return app_slot_bytes(fp.read())


def _program_bytes(env, elf):
    """The ELF size PlatformIO's own check computes, or None if it cannot."""
    pattern = env.get("SIZEPROGREGEXP")
    if not env.get("SIZETOOL") or not pattern:
        return None
    sysenv = dict(os.environ, PATH=str(env["ENV"]["PATH"]))
    try:
        out = subprocess.run([env.subst("$SIZETOOL"), "-A", "-d", elf], env=sysenv,
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    regexp = re.compile(pattern)
    total = 0
    for line in out.splitlines():
        m = regexp.search(line.strip())
        if m:
            total += sum(int(g) for g in m.groups())
    return total


def check_program(target, source, env):
    elf = source[0].get_abspath()
    program = _program_bytes(env, elf)
    slot = _slot(env)
    if program is None or program <= slot:
        return 0
    with open(elf, "rb") as fp:
        reason = _logo_reason(env.subst("$PROJECT_DIR"), elf=fp.read())
    sys.stderr.write("\n*** The linked program alone is %s bytes; the app slot holds %s (0x%X).\n%s\n\n" % (
        format(program, ","), format(slot, ","), slot, explain_too_big(reason)))
    return 1


def check_image(target, source, env):
    path = target[0].get_abspath()
    size = os.path.getsize(path)
    level, message = assess(size, _slot(env))
    if level == "ok":
        print("Size guard: " + message)
        return 0
    if level == "warn":
        print("*** Size guard warning: " + message)
        return 0
    with open(path, "rb") as fp:
        image = fp.read()
    elf_path = os.path.splitext(path)[0] + ".elf"
    elf = None
    if os.path.isfile(elf_path):
        with open(elf_path, "rb") as fp:
            elf = fp.read()
    reason = _logo_reason(env.subst("$PROJECT_DIR"), image=image, elf=elf)
    os.remove(path)
    sys.stderr.write("\n*** Size guard: %s\n%s\n*** Removed %s so it cannot be flashed.\n\n" % (
        message, explain_too_big(reason), path))
    return 1


if env is not None:
    env.AddPreAction("checkprogsize", env.VerboseAction(check_program, "Checking the program fits the app slot"))
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin",
                      env.VerboseAction(check_image, "Checking firmware.bin fits the app slot"))
