#!/usr/bin/env python3
"""
Sign a firmware build and lay out site/ for GitHub Pages. The maintainer runs
this on their own computer -- the private key never goes to CI. The whole
procedure is in docs/RELEASING.md; what a radar does with the result is in
docs/OTA.md.

    python3 scripts/make_release.py --version v2.1.0 \\
        --key ~/.config/plane-radar/ota-signing-key.pem --notes "Larger labels"

reads .pio/build/supermini/{firmware,bootloader,partitions}.bin, firmware.elf
if it is there, and the framework's boot_app0.bin, and writes

    site/ota/manifest.json          what radars poll
    site/ota/firmware-v2.1.0.bin    the signed image (older ones are removed)
    site/manifest.json              ESP Web Tools: the four parts of a fresh install
    site/flash/bootloader.bin, partitions.bin, boot_app0.bin

It writes nothing, and says why, when
  - the version is not a release ("v2.1.0"; not "-dirty", not "-3-g1a2b3c4"),
    is not newer than the one already in site/, or is not the version the
    image was built as;
  - the image still carries the airline logos (trademarks: never published);
  - the image is larger than an app slot, or partitions.bin is not the
    two-slot release layout (so a private build cannot get through);
  - include/ota_public_key.h is still the placeholder, does not match --key,
    or the image does not embed it -- radars running it could never take
    another update. --allow-missing-ota-key overrides only the last of these,
    for an image published before the updater exists.

    --check-only        run the checks on a build, sign nothing (CI)
    --verify-site DIR   check a laid-out site the way a radar will (pages.yml)

Needs the 'cryptography' package (scripts/requirements-release.txt) to sign or
verify; --check-only does not.
"""

import argparse
import base64
import getpass
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
import ota_common  # noqa: E402

DEFAULT_BUILD_DIR = ota_common.REPO_ROOT / ".pio" / "build" / "supermini"
DEFAULT_SITE = ota_common.REPO_ROOT / "site"
BOOT_APP0_IN_FRAMEWORK = Path("packages", "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")

# Free heap a radar should have before it starts an update: a TLS handshake
# (services::tls asks for 48 KB) plus Update's 4 KB sector buffer and margin.
DEFAULT_MIN_FREE_HEAP = 56 * 1024
# The radar parses the manifest into a small fixed buffer.
MANIFEST_MAX_BYTES = 1024
NOTES_MAX_CHARS = 200
HASH_CHUNK = 4096


class Refused(Exception):
    pass


# --- Inputs ----------------------------------------------------------------------

def default_boot_app0():
    roots = []
    if os.environ.get("PLATFORMIO_CORE_DIR"):
        roots.append(Path(os.environ["PLATFORMIO_CORE_DIR"]))
    roots.append(Path.home() / ".platformio")
    for root in roots:
        candidate = root / BOOT_APP0_IN_FRAMEWORK
        if candidate.is_file():
            return candidate
    return None


def read_build(args):
    """The four images (and the ELF, if any) as a dict of bytes."""
    build_dir = Path(args.build_dir)
    paths = {
        "app": Path(args.firmware) if args.firmware else build_dir / "firmware.bin",
        "bootloader": Path(args.bootloader) if args.bootloader else build_dir / "bootloader.bin",
        "partitions": Path(args.partitions) if args.partitions else build_dir / "partitions.bin",
        "boot_app0": Path(args.boot_app0) if args.boot_app0 else default_boot_app0(),
    }
    if paths["boot_app0"] is None:
        raise Refused("cannot find boot_app0.bin in the PlatformIO framework; pass --boot-app0")
    files = {}
    for name, path in paths.items():
        if not path.is_file():
            raise Refused("%s not found: %s (build first: pio run -e supermini)" % (name, path))
        files[name] = path.read_bytes()
    elf = Path(args.elf) if args.elf else paths["app"].with_suffix(".elf")
    files["elf"] = elf.read_bytes() if elf.is_file() else None
    return files


# --- Checks ----------------------------------------------------------------------

def check_build(files, version):
    """Problems that make this build unpublishable, as a list of strings."""
    problems = []
    image = files["app"]
    problems += ["firmware.bin " + p for p in ota_common.check_app_image(image)]
    if len(image) > ota_common.APP_SLOT_BYTES:
        problems.append("firmware.bin is %d bytes, more than the %d-byte app slot (0x%X)"
                        % (len(image), ota_common.APP_SLOT_BYTES, ota_common.APP_SLOT_BYTES))
    logos = ota_common.find_logos(image, files.get("elf"))
    if logos:
        problems.append("firmware.bin %s. Airline logos are trademarks and are never published; "
                        "build without them." % logos)
    if version is not None:
        if ota_common.parse_release(version) is None:
            problems.append("version %r is not a release (vMAJOR.MINOR.PATCH, built at its tag "
                            "from a clean checkout)" % version)
        elif ota_common.version_marker(version) not in image:
            problems.append("firmware.bin was not built as %s (its User-Agent says otherwise); "
                            "check out the tag and rebuild" % version)
    boot = files["bootloader"]
    problems += ["bootloader.bin " + p for p in ota_common.check_app_image(boot)]
    if len(boot) > ota_common.FLASH_OFFSETS["partitions"]:
        problems.append("bootloader.bin is %d bytes and would overwrite the partition table" % len(boot))
    problems += ["partitions.bin " + p for p in ota_common.check_release_layout(files["partitions"])]
    boot_app0 = files["boot_app0"]
    if len(boot_app0) != ota_common.BOOT_APP0_BYTES or struct.unpack_from("<I", boot_app0, 0)[0] != 1:
        problems.append("boot_app0.bin is not the framework's otadata image (8 KB, selecting app0)")
    return problems


def check_embedded_key(image, allow_missing):
    """
    (problems, warnings) about the public key the image will check its next
    update against: the committed one must be real, and the image must carry it.
    """
    try:
        placeholder, point = ota_common.read_public_key_header()
    except (OSError, ValueError) as err:
        return ["include/ota_public_key.h: %s" % err], []
    if placeholder:
        return ["include/ota_public_key.h is the placeholder: run scripts/ota_keygen.py first "
                "(docs/RELEASING.md)"], []
    if point in image:
        return [], []
    message = ("firmware.bin does not embed the public key %s, so a radar running it could never "
               "verify another update" % ota_common.fingerprint(point))
    if allow_missing:
        return [], [message + " (--allow-missing-ota-key)"]
    return [message + ". Build with the updater, or pass --allow-missing-ota-key if that is "
            "really what you want."], []


# --- Signing and verifying ---------------------------------------------------------

def load_private_key(path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    try:
        data = Path(path).expanduser().read_bytes()
    except OSError as err:
        raise Refused("cannot read the key: %s" % err)
    try:
        try:
            key = serialization.load_pem_private_key(data, password=None)
        except TypeError:  # encrypted
            passphrase = getpass.getpass("Passphrase for %s: " % path).encode("utf-8")
            key = serialization.load_pem_private_key(data, password=passphrase)
    except ValueError:
        raise Refused("%s: wrong passphrase, or not a PEM private key" % path)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        raise Refused("%s is not an ECDSA P-256 private key" % path)
    return key


def public_point(key):
    from cryptography.hazmat.primitives import serialization

    return key.public_key().public_bytes(serialization.Encoding.X962,
                                         serialization.PublicFormat.UncompressedPoint)


def sign(key, image):
    """ASN.1 DER ECDSA signature over SHA-256(image)."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec

    return key.sign(image, ec.ECDSA(hashes.SHA256()))


def verify_like_device(manifest, image, point):
    """
    What a radar checks before it lets an image boot (docs/OTA.md), as a list
    of problems. Like the radar, it hashes the image in chunks as they would
    arrive and checks the signature over that digest.
    """
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec, utils

    problems = []
    version = manifest.get("version")
    if not isinstance(version, str) or ota_common.parse_release(version) is None:
        return ["manifest version %r is not a release" % (version,)]
    size = manifest.get("size")
    if not isinstance(size, int) or size <= 0 or size > ota_common.APP_SLOT_BYTES:
        problems.append("manifest size %r is not 1..%d" % (size, ota_common.APP_SLOT_BYTES))
    if size != len(image):
        problems.append("manifest size %r but the image is %d bytes" % (size, len(image)))
    digest = hashlib.sha256()
    for pos in range(0, len(image), HASH_CHUNK):
        digest.update(image[pos:pos + HASH_CHUNK])
    if manifest.get("sha256") != digest.hexdigest():
        problems.append("manifest sha256 does not match the image")
    try:
        signature = base64.b64decode(manifest.get("sig", ""), validate=True)
    except (ValueError, TypeError):
        signature = b""
    try:
        key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), point)
        key.verify(signature, digest.digest(), ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    except (InvalidSignature, ValueError):
        problems.append("signature does not verify against public key %s" % ota_common.fingerprint(point))
    if ota_common.version_marker(version) not in image:
        problems.append("the image does not carry the version the manifest claims (%s)" % version)
    return problems


# --- Site --------------------------------------------------------------------------

def firmware_name(version):
    return "firmware-%s.bin" % version


def ota_manifest(version, image, signature, min_free_heap, notes):
    return {
        "version": version,
        "size": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "sig": base64.b64encode(signature).decode("ascii"),
        "min_free_heap_hint": min_free_heap,
        "notes": notes,
    }


def flasher_manifest(version):
    """ESP Web Tools manifest: a fresh install writes all four parts."""
    paths = {
        "bootloader": "flash/bootloader.bin",
        "partitions": "flash/partitions.bin",
        "boot_app0": "flash/boot_app0.bin",
        "app": "ota/" + firmware_name(version),
    }
    return {
        "name": "Plane Radar",
        "version": version,
        "new_install_prompt_erase": True,
        # The firmware does not speak Improv; do not wait for it after flashing.
        "new_install_improv_wait_time": 0,
        "builds": [{
            "chipFamily": "ESP32-C3",
            "parts": [{"path": paths[name], "offset": offset}
                      for name, offset in sorted(ota_common.FLASH_OFFSETS.items(), key=lambda kv: kv[1])],
        }],
    }


def dump_json(obj):
    return json.dumps(obj, indent=2, ensure_ascii=True) + "\n"


def published_version(site):
    path = Path(site) / "ota" / "manifest.json"
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8")).get("version")
    except ValueError:
        return None


def check_notes(notes):
    if len(notes) > NOTES_MAX_CHARS:
        raise Refused("--notes is %d characters; keep it under %d" % (len(notes), NOTES_MAX_CHARS))
    if any(ord(c) < 0x20 or ord(c) == 0x7F for c in notes):
        raise Refused("--notes must be one line of plain text")


def write_site(site, version, files, manifest):
    site = Path(site)
    ota_dir = site / "ota"
    flash_dir = site / "flash"
    ota_dir.mkdir(parents=True, exist_ok=True)
    flash_dir.mkdir(parents=True, exist_ok=True)
    # Images first and manifests last, so the site never names a file that is not there yet.
    (ota_dir / firmware_name(version)).write_bytes(files["app"])
    for name in ("bootloader", "partitions", "boot_app0"):
        (flash_dir / (name + ".bin")).write_bytes(files[name])
    (site / "manifest.json").write_text(dump_json(flasher_manifest(version)), encoding="utf-8")
    (ota_dir / "manifest.json").write_text(dump_json(manifest), encoding="utf-8")
    removed = []
    for old in sorted(ota_dir.glob("firmware-*.bin")):
        if old.name != firmware_name(version):
            old.unlink()
            removed.append(old.name)
    return removed


def verify_site(site):
    """Problems with a laid-out site, checked against include/ota_public_key.h."""
    site = Path(site)
    ota_path = site / "ota" / "manifest.json"
    flasher_path = site / "manifest.json"
    if not ota_path.exists() and not flasher_path.exists():
        print("verify-site: no release published yet (no manifests); nothing to check.")
        return []
    if not ota_path.is_file() or not flasher_path.is_file():
        return ["one of ota/manifest.json and manifest.json is missing"]
    raw = ota_path.read_bytes()
    problems = []
    if len(raw) > MANIFEST_MAX_BYTES:
        problems.append("ota/manifest.json is %d bytes; radars read at most %d" % (len(raw), MANIFEST_MAX_BYTES))
    try:
        manifest = json.loads(raw)
        flasher = json.loads(flasher_path.read_bytes())
    except ValueError as err:
        return problems + ["manifest is not valid JSON: %s" % err]
    version = manifest.get("version")
    if not isinstance(version, str) or ota_common.parse_release(version) is None:
        return problems + ["ota/manifest.json version %r is not a release" % (version,)]
    image_path = site / "ota" / firmware_name(version)
    if not image_path.is_file():
        return problems + ["%s is missing" % image_path]
    image = image_path.read_bytes()
    placeholder, point = ota_common.read_public_key_header()
    if placeholder:
        problems.append("include/ota_public_key.h is the placeholder, yet a release is published")
    else:
        problems += verify_like_device(manifest, image, point)
    if flasher != flasher_manifest(version):
        problems.append("manifest.json is not the ESP Web Tools manifest make_release.py writes for %s" % version)
    flash = {}
    for name in ("bootloader", "partitions", "boot_app0"):
        path = site / "flash" / (name + ".bin")
        if not path.is_file():
            problems.append("%s is missing" % path)
            continue
        flash[name] = path.read_bytes()
    if len(flash) == 3:
        # Logos, slot size, layout and the version marker, as for a build.
        problems += check_build(dict(flash, app=image, elf=None), version)
    return problems


# --- Main --------------------------------------------------------------------------

def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Sign a firmware build and lay out site/ for GitHub Pages (docs/RELEASING.md).")
    parser.add_argument("--version", help="release being published, e.g. v2.1.0 (the tag the build was made at)")
    parser.add_argument("--key", help="private signing key (from ota_keygen.py)")
    parser.add_argument("--notes", default="", help="one line shown with the update (<= %d chars)" % NOTES_MAX_CHARS)
    parser.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR))
    parser.add_argument("--firmware", help="default: <build-dir>/firmware.bin")
    parser.add_argument("--elf", help="default: firmware.elf next to firmware.bin, if present")
    parser.add_argument("--bootloader", help="default: <build-dir>/bootloader.bin")
    parser.add_argument("--partitions", help="default: <build-dir>/partitions.bin")
    parser.add_argument("--boot-app0", help="default: from the PlatformIO framework package")
    parser.add_argument("--site", default=str(DEFAULT_SITE))
    parser.add_argument("--min-free-heap", type=int, default=DEFAULT_MIN_FREE_HEAP)
    parser.add_argument("--allow-missing-ota-key", action="store_true",
                        help="publish an image that does not embed the update key (it can never update itself)")
    parser.add_argument("--check-only", action="store_true", help="check the build, sign and write nothing")
    parser.add_argument("--verify-site", metavar="DIR", help="verify a laid-out site and exit")
    return parser.parse_args(argv)


def run(args):
    if args.verify_site:
        problems = verify_site(args.verify_site)
        if problems:
            raise Refused("\n".join(problems))
        if published_version(args.verify_site) is not None:
            print("verify-site: %s is consistent and its release verifies." % args.verify_site)
        return

    files = read_build(args)
    problems = check_build(files, args.version)
    key_problems, warnings = check_embedded_key(files["app"], args.allow_missing_ota_key)
    if args.check_only and args.version is None:
        # A CI build between releases: the signing key is the release's business.
        warnings += key_problems
        key_problems = []
    for warning in warnings:
        print("warning: " + warning)

    if args.check_only:
        if problems or key_problems:
            raise Refused("\n".join(problems + key_problems))
        print("check-only: the build is publishable (%d bytes, %.1f %% of the app slot)."
              % (len(files["app"]), 100.0 * len(files["app"]) / ota_common.APP_SLOT_BYTES))
        return

    if args.version is None or args.key is None:
        raise Refused("--version and --key are required to publish")
    if args.min_free_heap <= 0:
        raise Refused("--min-free-heap must be a positive number of bytes")
    check_notes(args.notes)
    if problems or key_problems:
        raise Refused("\n".join(problems + key_problems))
    current = published_version(args.site)
    if current is not None:
        current_parts = ota_common.parse_release(current)
        if current_parts is not None and ota_common.parse_release(args.version) <= current_parts:
            raise Refused("site/ already publishes %s; radars only install a newer version than that"
                          % current)

    key = load_private_key(args.key)
    point = public_point(key)
    _, header_point = ota_common.read_public_key_header()
    if point != header_point:
        raise Refused("--key is key %s, but include/ota_public_key.h holds %s: radars would reject "
                      "this signature" % (ota_common.fingerprint(point), ota_common.fingerprint(header_point)))
    image = files["app"]
    manifest = ota_manifest(args.version, image, sign(key, image), args.min_free_heap, args.notes)
    if len(dump_json(manifest).encode("utf-8")) > MANIFEST_MAX_BYTES:
        raise Refused("the manifest would exceed %d bytes; shorten --notes" % MANIFEST_MAX_BYTES)
    problems = verify_like_device(manifest, image, point)
    if problems:
        raise Refused("the fresh signature does not verify:\n" + "\n".join(problems))

    removed = write_site(args.site, args.version, files, manifest)
    print("Published %s into %s: %d bytes, sha256 %s, key %s."
          % (args.version, args.site, len(image), manifest["sha256"], ota_common.fingerprint(point)))
    for name in removed:
        print("Removed the previous image %s." % name)
    print("Next: git add site && git commit -m \"Release %s\" && git push origin main %s  (docs/RELEASING.md)"
          % (args.version, args.version))


def main(argv=None):
    try:
        run(parse_args(argv))
    except Refused as err:
        sys.stderr.write("make_release: refused:\n")
        for line in str(err).splitlines():
            sys.stderr.write("  " + line + "\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
