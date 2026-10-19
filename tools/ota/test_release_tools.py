"""
Host tests for the release tooling (scripts/ota_keygen.py, make_release.py,
size_guard.py, ota_common.py) and the contract in docs/OTA.md.

A throwaway key is generated in a temporary directory, a synthetic build is
signed with it, and the result is checked the way a radar will check it: the
image hashed in chunks as they would arrive over the network, then the ECDSA
P-256 signature verified over that digest against the public key read back
out of the generated C header. No real key and no real logo data are involved.

    pip install -r scripts/requirements-release.txt
    python3 -m unittest discover -s tools/ota -v
"""

import base64
import contextlib
import hashlib
import io
import json
import os
import re
import stat
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO / "scripts"))

import make_release  # noqa: E402
import ota_common  # noqa: E402
import ota_keygen  # noqa: E402
import size_guard  # noqa: E402

from cryptography.exceptions import InvalidSignature  # noqa: E402
from cryptography.hazmat.primitives import hashes, serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import ec, utils  # noqa: E402

VERSION = "v2.3.4"


# --- Synthetic build -------------------------------------------------------------

def encode_partition_table(rows):
    """partitions.bin as gen_esp32part.py writes it: 32-byte entries, an MD5 row, 0xFF to 3 KB."""
    blob = b""
    for label, ptype, subtype, offset, size in rows:
        blob += b"\xaa\x50" + struct.pack("<BBII", ptype, subtype, offset, size)
        blob += label.encode("ascii").ljust(16, b"\0") + struct.pack("<I", 0)
    blob += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(blob).digest()
    return blob.ljust(0xC00, b"\xff")


def app_image(size, extra=b"", seed=1):
    """Bytes shaped like an ESP32-C3 image: magic, chip id 5, then noise and `extra`."""
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 3
    struct.pack_into("<H", header, 12, ota_common.ESP32C3_CHIP_ID)
    body = bytearray()
    block = hashlib.sha256(b"seed%d" % seed).digest()
    while len(body) < size - len(header) - len(extra):
        block = hashlib.sha256(block).digest()
        body += block
    body = body[:size - len(header) - len(extra)]
    return bytes(header) + bytes(body) + extra


def logo_pixels(count):
    """`count` transparent logo pixels (kLogoKey 0x1FF8), as 32-pixel rows."""
    return b"\xf8\x1f" * count


class Build:
    """A build directory plus a header and a key, all in a temp dir."""

    def __init__(self, root, point, firmware=None, layout=ota_common.RELEASE_LAYOUT):
        self.dir = Path(root) / "build"
        self.dir.mkdir()
        marker = ota_common.version_marker(VERSION)
        self.firmware = firmware if firmware is not None else app_image(1_400_000, marker + point)
        (self.dir / "firmware.bin").write_bytes(self.firmware)
        (self.dir / "bootloader.bin").write_bytes(app_image(13_200, seed=2))
        (self.dir / "partitions.bin").write_bytes(encode_partition_table(layout))
        boot_app0 = struct.pack("<I", 1) + b"\xff" * (0x2000 - 4)
        self.boot_app0 = Path(root) / "boot_app0.bin"
        self.boot_app0.write_bytes(boot_app0)

    def args(self, *extra):
        return ["--build-dir", str(self.dir), "--boot-app0", str(self.boot_app0)] + list(extra)


def run_tool(module, argv):
    """(exit code, stdout, stderr) of a tool's main()."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = module.main(argv)
        except SystemExit as exit_:
            code = exit_.code
    return code, out.getvalue(), err.getvalue()


def verify_as_device(manifest, image, header_text):
    """
    What the radar does (docs/OTA.md): read the key out of the header, hash the
    image as it streams in, check the signature over the digest. Raises on
    failure.
    """
    placeholder, point = ota_common.parse_public_key_header(header_text)
    if placeholder:
        raise InvalidSignature("placeholder key")
    if manifest["size"] != len(image) or manifest["size"] > ota_common.APP_SLOT_BYTES:
        raise InvalidSignature("size")
    digest = hashlib.sha256()
    for pos in range(0, len(image), 1460):  # one TCP segment at a time
        digest.update(image[pos:pos + 1460])
    if digest.hexdigest() != manifest["sha256"]:
        raise InvalidSignature("sha256")
    key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), point)
    key.verify(base64.b64decode(manifest["sig"], validate=True), digest.digest(),
               ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    if ota_common.version_marker(manifest["version"]) not in image:
        raise InvalidSignature("version marker")


class ToolTest(unittest.TestCase):
    """Each test gets a temp dir with a throwaway key and its own header."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.key_path = self.root / "keys" / "throwaway.pem"
        self.key_path.parent.mkdir(mode=0o700)
        self.header = self.root / "ota_public_key.h"
        self.header.write_text(ota_common.render_public_key_header(None))
        patcher = mock.patch.object(ota_common, "PUBLIC_KEY_HEADER", self.header)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(self.tmp.cleanup)
        self.site = self.root / "site"

    def keygen(self):
        code, out, err = run_tool(ota_keygen, [str(self.key_path), "--no-passphrase"])
        self.assertEqual(code, 0, err)
        _, point = ota_common.read_public_key_header()
        return point

    def release(self, build, *extra, version=VERSION):
        return run_tool(make_release, build.args("--version", version, "--key", str(self.key_path),
                                                 "--site", str(self.site), *extra))


# --- Contract between files --------------------------------------------------------

class ContractTest(unittest.TestCase):
    def test_release_layout_is_the_committed_partition_table(self):
        rows = []
        for line in (REPO / "partitions" / "plane_radar.csv").read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                rows.append([c.strip() for c in line.split(",")])
        types = {"app": 0, "data": 1}
        subtypes = {"nvs": 0x02, "ota": 0x00, "ota_0": 0x10, "ota_1": 0x11, "coredump": 0x03}
        got = tuple((r[0], types[r[1]], subtypes.get(r[2]) if r[2] in subtypes else int(r[2], 0),
                     int(r[3], 0), int(r[4], 0)) for r in rows)
        self.assertEqual(got, ota_common.RELEASE_LAYOUT)

    def test_layout_is_aligned_contiguous_and_fills_4mb(self):
        end = 0x9000
        for label, ptype, _, offset, size in ota_common.RELEASE_LAYOUT:
            self.assertEqual(offset, end, label)
            self.assertEqual(offset % (0x10000 if ptype == 0 else 0x1000), 0, label)
            end = offset + size
        self.assertEqual(end, 4 * 1024 * 1024)
        self.assertEqual(ota_common.RELEASE_LAYOUT[0], ("nvs", 1, 0x02, 0x9000, 0x5000))

    def test_size_guard_reads_the_slot_from_the_table(self):
        text = (REPO / "partitions" / "plane_radar.csv").read_text()
        self.assertEqual(size_guard.app_slot_bytes(text), ota_common.APP_SLOT_BYTES)
        self.assertEqual(size_guard.WARN_FRACTION, ota_common.SLOT_WARN_FRACTION)

    def test_marker_matches_the_user_agent_in_config_h(self):
        config = (REPO / "include" / "config.h").read_text()
        url = re.search(r'#define PLANE_RADAR_REPO_URL "([^"]+)"', config).group(1)
        self.assertEqual(url, ota_common.REPO_URL)
        self.assertIn('kUserAgent[] = "PlaneRadar/" PLANE_RADAR_VERSION " (+" PLANE_RADAR_REPO_URL ")";',
                      config)
        self.assertEqual(ota_common.version_marker("v2.1.0"),
                         b"PlaneRadar/v2.1.0 (+https://github.com/ericcheng0904-pixel/plane-radar)")

    def test_committed_header_is_generated(self):
        text = (REPO / "include" / "ota_public_key.h").read_text()
        placeholder, point = ota_common.parse_public_key_header(text)
        self.assertEqual(text, ota_common.render_public_key_header(None if placeholder else point))

    def test_version_grammar_matches_the_device(self):
        # The same strings as test/test_ota/test_main.cpp.
        for good in ("v2.1.0", "v0.0.0", "v65535.65535.65535", "v10.20.30"):
            self.assertIsNotNone(ota_common.parse_release(good), good)
        for bad in ("v2.1.0-dirty", "v2.1.0-3-g1a2b3c4", "v2.1.0-private", "1a2b3c4", "dev", "",
                    "2.1.0", "V2.1.0", "v2.1", "v2.1.0.0", "v2..0", "v.1.0", "v2.1.", "v02.1.0",
                    "v2.01.0", "v2.1.00", "v65536.0.0", "v999999.0.0", "v2.1.0 ", " v2.1.0",
                    "v2.1.0/../x", "v2.1.-1", "v+2.1.0", "v2.1.0\n", None):
            self.assertIsNone(ota_common.parse_release(bad), repr(bad))
        self.assertLess(ota_common.parse_release("v2.9.0"), ota_common.parse_release("v2.10.0"))


# --- Building blocks ---------------------------------------------------------------

class GuardTest(unittest.TestCase):
    slot = ota_common.APP_SLOT_BYTES

    def test_logo_free_image_is_ok(self):
        self.assertEqual(size_guard.assess(1_493_856, self.slot)[0], "ok")

    def test_warns_above_ninety_percent(self):
        self.assertEqual(size_guard.assess(int(self.slot * 0.90), self.slot)[0], "ok")
        self.assertEqual(size_guard.assess(int(self.slot * 0.90) + 1, self.slot)[0], "warn")
        self.assertEqual(size_guard.assess(self.slot, self.slot)[0], "warn")

    def test_fails_one_byte_over(self):
        level, message = size_guard.assess(self.slot + 1, self.slot)
        self.assertEqual(level, "fail")
        self.assertIn("1 bytes too many", message)

    def test_image_with_logos_fails_and_says_why(self):
        self.assertEqual(size_guard.assess(1_913_632, self.slot)[0], "fail")
        text = size_guard.explain_too_big("contains airline logo pixels")
        self.assertIn("contains airline logo pixels", text)
        self.assertIn("logo pack", text)
        self.assertNotIn("PRIVATE", text.upper())

    def test_too_big_message_does_not_suggest_another_layout(self):
        text = size_guard.explain_too_big(None)
        self.assertIn("two app slots", text)
        self.assertNotIn("PRIVATE", text.upper())
        self.assertNotIn("single-slot", text)


class LogoTest(unittest.TestCase):
    def test_logo_pixels_are_found(self):
        image = app_image(200_000, logo_pixels(32) * 40)
        self.assertIsNotNone(ota_common.find_logos(image))

    def test_logo_free_image_passes(self):
        self.assertIsNone(ota_common.find_logos(app_image(1_400_000)))

    def test_one_small_icon_on_the_key_colour_passes(self):
        icon = (logo_pixels(16) + b"\x00\x00" * 16) * 8  # half of a 16x16 icon transparent
        self.assertIsNone(ota_common.find_logos(app_image(200_000, icon)))

    def test_logo_symbols_in_the_elf_are_found(self):
        elf = b"\0_ZN4data14airline_brands12_GLOBAL__N_1L6kAAL32E\0"
        self.assertIn("links logo arrays (kAAL32)", ota_common.find_logos(app_image(1000), elf))


class PartitionTableTest(unittest.TestCase):
    def test_release_layout_round_trips(self):
        self.assertEqual(ota_common.check_release_layout(encode_partition_table(ota_common.RELEASE_LAYOUT)), [])

    def test_private_layout_is_refused(self):
        private = (
            ("nvs", 1, 0x02, 0x9000, 0x5000), ("otadata", 1, 0x00, 0xE000, 0x2000),
            ("app0", 0, 0x10, 0x10000, 0x300000), ("spiffs", 1, 0x82, 0x310000, 0xE0000),
            ("coredump", 1, 0x03, 0x3F0000, 0x10000))
        problems = ota_common.check_release_layout(encode_partition_table(private))
        self.assertEqual(len(problems), 1)
        self.assertIn("not the release layout", problems[0])

    def test_corrupt_table_is_refused(self):
        blob = bytearray(encode_partition_table(ota_common.RELEASE_LAYOUT))
        blob[8] ^= 0x01  # one bit of app0's offset
        self.assertIn("MD5", ota_common.check_release_layout(bytes(blob))[0])


# --- Key generation ---------------------------------------------------------------

class KeygenTest(ToolTest):
    def test_writes_a_private_key_only_the_owner_can_read(self):
        point = self.keygen()
        mode = stat.S_IMODE(os.stat(self.key_path).st_mode)
        self.assertEqual(mode, 0o600)
        key = serialization.load_pem_private_key(self.key_path.read_bytes(), password=None)
        self.assertEqual(make_release.public_point(key), point)
        placeholder, _ = ota_common.read_public_key_header()
        self.assertFalse(placeholder)
        self.assertEqual(point[0], 0x04)
        self.assertEqual(self.header.read_text(), ota_common.render_public_key_header(point))

    def test_refuses_a_path_inside_the_repository(self):
        inside = REPO / "build-key-must-not-exist.pem"
        code, _, err = run_tool(ota_keygen, [str(inside), "--no-passphrase"])
        self.assertEqual(code, 1)
        self.assertIn("inside this repository", err)
        self.assertFalse(inside.exists())

    def test_never_overwrites_a_key(self):
        self.keygen()
        before = self.key_path.read_bytes()
        code, _, err = run_tool(ota_keygen, [str(self.key_path), "--no-passphrase"])
        self.assertEqual(code, 1)
        self.assertIn("already exists", err)
        self.assertEqual(self.key_path.read_bytes(), before)

    def test_keeps_a_real_public_key_unless_told(self):
        point = self.keygen()
        other = self.root / "keys" / "second.pem"
        code, _, err = run_tool(ota_keygen, [str(other), "--no-passphrase"])
        self.assertEqual(code, 1)
        self.assertIn("--replace-key", err)
        self.assertFalse(other.exists())
        self.assertEqual(ota_common.read_public_key_header()[1], point)

    def test_without_a_terminal_a_passphrase_is_not_guessed(self):
        with mock.patch.object(sys.stdin, "isatty", return_value=False):
            code, _, err = run_tool(ota_keygen, [str(self.key_path)])
        self.assertEqual(code, 1)
        self.assertIn("--no-passphrase", err)
        self.assertFalse(self.key_path.exists())


# --- Publishing -------------------------------------------------------------------

class ReleaseTest(ToolTest):
    def test_signed_release_verifies_as_the_device_will(self):
        point = self.keygen()
        build = Build(self.root, point)
        code, out, err = self.release(build, "--notes", "Larger labels")
        self.assertEqual(code, 0, err)

        manifest = json.loads((self.site / "ota" / "manifest.json").read_text())
        self.assertEqual(list(manifest), ["version", "size", "sha256", "sig", "min_free_heap_hint", "notes"])
        self.assertEqual(manifest["version"], VERSION)
        self.assertEqual(manifest["notes"], "Larger labels")
        self.assertLessEqual(len((self.site / "ota" / "manifest.json").read_bytes()), 1024)
        image = (self.site / "ota" / ("firmware-%s.bin" % VERSION)).read_bytes()
        self.assertEqual(image, build.firmware)
        verify_as_device(manifest, image, self.header.read_text())

        # One flipped bit anywhere and the radar must refuse it.
        tampered = bytearray(image)
        tampered[len(tampered) // 2] ^= 0x01
        with self.assertRaises(InvalidSignature):
            verify_as_device(manifest, bytes(tampered), self.header.read_text())
        with self.assertRaises(InvalidSignature):
            verify_as_device(dict(manifest, sha256=hashlib.sha256(tampered).hexdigest()),
                             bytes(tampered), self.header.read_text())

        # A different key must not verify it either.
        stranger = ec.generate_private_key(ec.SECP256R1()).public_key().public_bytes(
            serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
        with self.assertRaises(InvalidSignature):
            verify_as_device(manifest, image, ota_common.render_public_key_header(stranger))

        self.assertEqual(make_release.verify_site(self.site), [])

    def test_web_flasher_manifest_has_the_four_parts(self):
        build = Build(self.root, self.keygen())
        self.assertEqual(self.release(build)[0], 0)
        flasher = json.loads((self.site / "manifest.json").read_text())
        self.assertTrue(flasher["new_install_prompt_erase"])
        parts = flasher["builds"][0]["parts"]
        self.assertEqual(flasher["builds"][0]["chipFamily"], "ESP32-C3")
        self.assertEqual([(p["path"], p["offset"]) for p in parts], [
            ("flash/bootloader.bin", 0x0), ("flash/partitions.bin", 0x8000),
            ("flash/boot_app0.bin", 0xE000), ("ota/firmware-%s.bin" % VERSION, 0x10000)])
        for part in parts:
            self.assertTrue((self.site / part["path"]).is_file(), part["path"])

    def test_next_release_replaces_the_previous_image(self):
        point = self.keygen()
        self.assertEqual(self.release(Build(self.root, point))[0], 0)
        newer = "v2.4.0"
        firmware = app_image(1_400_000, ota_common.version_marker(newer) + point, seed=9)
        (self.root / "next").mkdir()
        code, out, err = self.release(Build(self.root / "next", point, firmware), version=newer)
        self.assertEqual(code, 0, err)
        self.assertEqual(sorted(p.name for p in (self.site / "ota").glob("*.bin")), ["firmware-v2.4.0.bin"])
        self.assertEqual(make_release.verify_site(self.site), [])

    def test_encrypted_key_asks_for_its_passphrase(self):
        point = self.keygen()
        key = serialization.load_pem_private_key(self.key_path.read_bytes(), password=None)
        encrypted = self.root / "keys" / "encrypted.pem"
        encrypted.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                                serialization.BestAvailableEncryption(b"correct horse")))
        self.key_path = encrypted
        with mock.patch("getpass.getpass", return_value="correct horse"):
            code, _, err = self.release(Build(self.root, point))
        self.assertEqual(code, 0, err)

    def assertRefused(self, result, *needles):
        code, _, err = result
        self.assertEqual(code, 1, "expected a refusal")
        for needle in needles:
            self.assertIn(needle, err)
        self.assertFalse((self.site / "ota" / "manifest.json").exists(), "a refusal must write nothing")

    def test_refuses_a_development_version(self):
        build = Build(self.root, self.keygen())
        self.assertRefused(self.release(build, version="v2.3.4-dirty"), "not a release")
        self.assertRefused(self.release(build, version="v2.3.4-1-g1a2b3c4"), "not a release")

    def test_refuses_a_version_the_image_was_not_built_as(self):
        build = Build(self.root, self.keygen())
        self.assertRefused(self.release(build, version="v2.3.5"), "was not built as v2.3.5")

    def test_refuses_an_image_with_logos(self):
        point = self.keygen()
        firmware = app_image(1_400_000, logo_pixels(32) * 64 + ota_common.version_marker(VERSION) + point)
        self.assertRefused(self.release(Build(self.root, point, firmware)), "logo", "trademarks")

    def test_refuses_an_image_larger_than_the_slot(self):
        point = self.keygen()
        firmware = app_image(ota_common.APP_SLOT_BYTES + 1, ota_common.version_marker(VERSION) + point)
        self.assertRefused(self.release(Build(self.root, point, firmware)), "more than the")

    def test_refuses_a_private_layout(self):
        private = (("nvs", 1, 0x02, 0x9000, 0x5000), ("otadata", 1, 0x00, 0xE000, 0x2000),
                   ("app0", 0, 0x10, 0x10000, 0x300000))
        build = Build(self.root, self.keygen(), layout=private)
        self.assertRefused(self.release(build), "not the release layout")

    def test_refuses_while_the_header_is_the_placeholder(self):
        build = Build(self.root, bytes(65))
        code, _, err = run_tool(ota_keygen, [str(self.key_path), "--no-passphrase",
                                             "--header", str(self.root / "elsewhere.h")])
        self.assertEqual(code, 0, err)
        self.assertRefused(self.release(build), "placeholder")

    def test_refuses_a_key_that_is_not_the_committed_one(self):
        point = self.keygen()
        build = Build(self.root, point)
        self.key_path = self.root / "keys" / "other.pem"
        run_tool(ota_keygen, [str(self.key_path), "--no-passphrase", "--header", str(self.root / "other.h")])
        self.assertRefused(self.release(build), "would reject this signature")

    def test_refuses_an_image_that_cannot_verify_its_next_update(self):
        point = self.keygen()
        firmware = app_image(1_400_000, ota_common.version_marker(VERSION))  # no key inside
        build = Build(self.root, point, firmware)
        self.assertRefused(self.release(build), "does not embed the public key")
        code, out, err = self.release(build, "--allow-missing-ota-key")
        self.assertEqual(code, 0, err)
        self.assertIn("--allow-missing-ota-key", out)

    def test_refuses_to_publish_the_same_version_twice(self):
        build = Build(self.root, self.keygen())
        self.assertEqual(self.release(build)[0], 0)
        code, _, err = self.release(build)
        self.assertEqual(code, 1)
        self.assertIn("already publishes v2.3.4", err)

    def test_refuses_long_or_multiline_notes(self):
        build = Build(self.root, self.keygen())
        self.assertRefused(self.release(build, "--notes", "x" * 201), "--notes")
        self.assertRefused(self.release(build, "--notes", "two\nlines"), "--notes")

    def test_check_only_needs_no_key_and_signs_nothing(self):
        build = Build(self.root, self.keygen())
        code, out, err = run_tool(make_release, build.args("--check-only", "--site", str(self.site)))
        self.assertEqual(code, 0, err)
        self.assertIn("publishable", out)
        self.assertFalse(self.site.exists())


class VerifySiteTest(ToolTest):
    def test_empty_site_is_fine(self):
        self.site.mkdir()
        (self.site / "index.html").write_text("<!doctype html>")
        self.assertEqual(make_release.verify_site(self.site), [])

    def test_committed_site_verifies(self):
        self.assertEqual(make_release.verify_site(REPO / "site") if (REPO / "site").is_dir() else [], [])

    def test_tampered_site_is_caught(self):
        point = self.keygen()
        self.assertEqual(self.release(Build(self.root, point))[0], 0)
        image_path = self.site / "ota" / ("firmware-%s.bin" % VERSION)
        image = bytearray(image_path.read_bytes())
        image[1000] ^= 0xFF
        image_path.write_bytes(bytes(image))
        problems = make_release.verify_site(self.site)
        self.assertTrue(any("sha256" in p for p in problems), problems)
        self.assertTrue(any("signature" in p for p in problems), problems)

    def test_placeholder_key_with_a_published_release_is_caught(self):
        point = self.keygen()
        self.assertEqual(self.release(Build(self.root, point))[0], 0)
        self.header.write_text(ota_common.render_public_key_header(None))
        self.assertTrue(any("placeholder" in p for p in make_release.verify_site(self.site)))


if __name__ == "__main__":
    unittest.main()
