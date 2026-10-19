"""
What the release tools (ota_keygen.py, make_release.py, size_guard.py) agree
on with each other and with the firmware: the flash layout, the version
grammar, the public-key header, and how to tell an image that still carries
the airline logos. docs/OTA.md is the contract these encode.

Standard library only: size_guard.py imports this inside PlatformIO's Python,
which does not have the 'cryptography' package. Signing lives in
make_release.py.
"""

import hashlib
import re
import struct
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PUBLIC_KEY_HEADER = REPO_ROOT / "include" / "ota_public_key.h"

# Mirrors PLANE_RADAR_REPO_URL in include/config.h (a host test compares them).
REPO_URL = "https://github.com/ericcheng0904-pixel/plane-radar"

# --- Flash layout (partitions/plane_radar.csv) -------------------------------

APP_SLOT_BYTES = 0x1C0000
# Above this share of the slot the build warns: the next few releases will not fit.
SLOT_WARN_FRACTION = 0.90

# (label, type, subtype, offset, size): what partitions.bin must decode to
# before anything built with it is published. Type 0 = app, 1 = data.
RELEASE_LAYOUT = (
    ("nvs", 1, 0x02, 0x9000, 0x5000),
    ("otadata", 1, 0x00, 0xE000, 0x2000),
    ("app0", 0, 0x10, 0x10000, APP_SLOT_BYTES),
    ("app1", 0, 0x11, 0x1D0000, APP_SLOT_BYTES),
    ("logos", 1, 0x40, 0x390000, 0x60000),
    ("coredump", 1, 0x03, 0x3F0000, 0x10000),
)

# Where each piece goes on a fresh install (ESP Web Tools manifest, merge_firmware.py).
FLASH_OFFSETS = {
    "bootloader": 0x0,
    "partitions": 0x8000,
    "boot_app0": 0xE000,
    "app": 0x10000,
}
BOOT_APP0_BYTES = 0x2000

# --- Versions (include/util/release_version.h) ---------------------------------

# "v<major>.<minor>.<patch>", no leading zeros; each part must also be <= 65535.
# Matched with fullmatch: "$" would let a trailing newline through.
_RELEASE_RE = re.compile(r"v(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})")


def parse_release(text):
    """(major, minor, patch) for a release version, else None. Same grammar as
    util::release_version::parse on the device."""
    m = _RELEASE_RE.fullmatch(text or "")
    if not m:
        return None
    parts = tuple(int(g) for g in m.groups())
    if any(p > 65535 for p in parts):
        return None
    return parts


def version_marker(version):
    """
    The bytes every image built as `version` contains: config::kUserAgent. The
    signature covers the image but not the manifest, so the device checks the
    image it downloaded carries the version the manifest claims; without that,
    whoever controls the manifest could relabel an old signed image as new.
    """
    return ("PlaneRadar/%s (+%s)" % (version, REPO_URL)).encode("ascii")


# --- Application image ---------------------------------------------------------

ESP_IMAGE_MAGIC = 0xE9
ESP32C3_CHIP_ID = 5


def check_app_image(image):
    """Problems with `image` as an ESP32-C3 application, as a list of strings."""
    problems = []
    if len(image) < 24:
        return ["is %d bytes, too short to be an application image" % len(image)]
    if image[0] != ESP_IMAGE_MAGIC:
        problems.append("does not start with the image magic byte 0xE9")
    chip_id = struct.unpack_from("<H", image, 12)[0]
    if chip_id != ESP32C3_CHIP_ID:
        problems.append("is built for chip id %d, not the ESP32-C3 (%d)" % (chip_id, ESP32C3_CHIP_ID))
    return problems


# --- Partition table -----------------------------------------------------------

_PART_MAGIC = b"\xaa\x50"
_MD5_MAGIC = b"\xeb\xeb"


def parse_partition_table(blob):
    """
    Decode partitions.bin (the format gen_esp32part.py writes) into
    (label, type, subtype, offset, size, flags) tuples. Raises ValueError when
    the table is malformed or its MD5 entry does not match.
    """
    entries = []
    md5_seen = False
    for pos in range(0, len(blob) - 31, 32):
        row = blob[pos:pos + 32]
        if row[:2] == _PART_MAGIC:
            ptype, subtype, offset, size = struct.unpack_from("<BBII", row, 2)
            label = row[12:28].split(b"\0", 1)[0].decode("ascii", "replace")
            flags = struct.unpack_from("<I", row, 28)[0]
            entries.append((label, ptype, subtype, offset, size, flags))
        elif row[:2] == _MD5_MAGIC:
            if hashlib.md5(blob[:pos]).digest() != row[16:32]:
                raise ValueError("partition table MD5 does not match its entries")
            md5_seen = True
            break
        elif row == b"\xff" * 32:
            break
        else:
            raise ValueError("unexpected bytes at offset 0x%x of the partition table" % pos)
    if not entries:
        raise ValueError("no partition entries found")
    if not md5_seen:
        raise ValueError("partition table has no MD5 entry")
    return entries


def check_release_layout(blob):
    """Problems with partitions.bin as the release layout, as a list of strings."""
    try:
        entries = parse_partition_table(blob)
    except ValueError as err:
        return [str(err)]
    got = tuple((label, ptype, subtype, offset, size) for label, ptype, subtype, offset, size, _ in entries)
    if got == RELEASE_LAYOUT:
        return []

    def fmt(rows):
        return "; ".join("%s %d/0x%02x @0x%x +0x%x" % row for row in rows)

    return ["is not the release layout (partitions/plane_radar.csv).\n"
            "      expected: %s\n      found:    %s" % (fmt(RELEASE_LAYOUT), fmt(got))]


# --- Airline logos -------------------------------------------------------------

# Logo pixels equal to data::airline_brands::kLogoKey (0x1FF8) are transparent.
# Stored as uint16_t, little-endian, so the margins of every logo are long runs
# of F8 1F. Code and other data almost never repeat one halfword 16 times.
_LOGO_KEY_RUN = re.compile(b"(?:\xf8\x1f){16,}")
# One 32x32 logo is 1024 pixels; the image with all 164 logos has ~63,000 key
# pixels in such runs, a logo-free image none. A small icon drawn on the same
# key would stay far below this.
LOGO_PIXEL_LIMIT = 1024
# Symbols the generator gives the logo arrays (kAAL32, kAAL16, ...).
_LOGO_SYMBOL = re.compile(rb"airline_brands\w*?L\d+(k[A-Z0-9]{3}(?:16|32))E")


def logo_key_pixels(image):
    """Number of transparent-key pixels in runs of 16 or more."""
    return sum(len(m.group()) // 2 for m in _LOGO_KEY_RUN.finditer(image))


def logo_symbols(elf):
    """Logo array names found in an ELF's symbol table (empty if none)."""
    return sorted({m.group(1).decode("ascii") for m in _LOGO_SYMBOL.finditer(elf)})


def find_logos(image, elf=None):
    """A sentence saying why `image` contains airline logos, or None."""
    pixels = logo_key_pixels(image)
    if pixels > LOGO_PIXEL_LIMIT:
        return ("contains airline logo pixels (%d transparent-key pixels in runs, "
                "limit %d)" % (pixels, LOGO_PIXEL_LIMIT))
    if elf is not None:
        names = logo_symbols(elf)
        if names:
            return "links logo arrays (%s%s)" % (", ".join(names[:4]), ", ..." if len(names) > 4 else "")
    return None


# --- Public key header (include/ota_public_key.h) --------------------------------

PUBLIC_POINT_BYTES = 65
_PLACEHOLDER_FINGERPRINT = "placeholder"


def fingerprint(point):
    """First 8 bytes of SHA-256 over the 65-byte public point, in hex."""
    return hashlib.sha256(point).hexdigest()[:16]


def render_public_key_header(point=None):
    """
    The text of include/ota_public_key.h for the 65-byte uncompressed point
    `point`, or the placeholder when `point` is None.
    """
    placeholder = point is None
    if placeholder:
        point = bytes(PUBLIC_POINT_BYTES)
    if len(point) != PUBLIC_POINT_BYTES:
        raise ValueError("public point must be %d bytes" % PUBLIC_POINT_BYTES)
    if not placeholder and point[0] != 0x04:
        raise ValueError("public point must be uncompressed (0x04 || X || Y)")
    rows = []
    for i in range(0, len(point), 12):
        rows.append("    " + ", ".join("0x%02x" % b for b in point[i:i + 12]) + ",")
    if placeholder:
        status = (
            " * THIS IS A PLACEHOLDER: no signing key has been generated yet. The point\n"
            " * below is all zeros, which is not on the curve, and\n"
            " * kOtaPublicKeyIsPlaceholder tells the updater to refuse every update\n"
            " * without trying. scripts/ota_keygen.py replaces this file.\n"
        )
        fp = _PLACEHOLDER_FINGERPRINT
    else:
        fp = fingerprint(point)
        status = (
            " * Key fingerprint %s. Replacing this key means radars running an\n"
            " * image built with it accept no update signed with the new one: they\n"
            " * would have to be flashed over USB.\n" % fp
        )
    return (
        "#pragma once\n"
        "\n"
        "/**\n"
        " * The public half of the key that signs over-the-air firmware updates.\n"
        " *\n"
        " * GENERATED by scripts/ota_keygen.py -- do not edit. The private half never\n"
        " * enters this repository (docs/RELEASING.md).\n"
        " *\n"
        " * kOtaPublicKey is an ECDSA P-256 (secp256r1) public key as an uncompressed\n"
        " * SEC1 point: 0x04, then X and Y, 32 bytes each, big-endian. mbedTLS loads\n"
        " * it with mbedtls_ecp_group_load(MBEDTLS_ECP_DP_SECP256R1) and\n"
        " * mbedtls_ecp_point_read_binary(). The manifest's \"sig\" is an ASN.1 DER\n"
        " * ECDSA signature over the SHA-256 of the firmware bytes, which\n"
        " * mbedtls_ecdsa_read_signature() checks. docs/OTA.md has the procedure.\n"
        " *\n"
        "%s"
        " */\n"
        "\n"
        "#include <cstdint>\n"
        "\n"
        "namespace config {\n"
        "\n"
        "constexpr bool kOtaPublicKeyIsPlaceholder = %s;\n"
        "\n"
        "constexpr uint8_t kOtaPublicKey[%d] = {\n"
        "%s\n"
        "};\n"
        "\n"
        "/** First 8 bytes of SHA-256(kOtaPublicKey) in hex: names the key in logs. */\n"
        "constexpr char kOtaPublicKeyFingerprint[] = \"%s\";\n"
        "\n"
        "}  // namespace config\n"
    ) % (status, "true" if placeholder else "false", PUBLIC_POINT_BYTES, "\n".join(rows), fp)


_HEADER_FLAG = re.compile(r"kOtaPublicKeyIsPlaceholder\s*=\s*(true|false)\s*;")
_HEADER_ARRAY = re.compile(r"kOtaPublicKey\[\s*\d+\s*\]\s*=\s*\{([^}]*)\}", re.S)


def parse_public_key_header(text):
    """
    (is_placeholder, point) from the text of include/ota_public_key.h. Raises
    ValueError if the file does not have the generated shape.
    """
    flag = _HEADER_FLAG.search(text)
    array = _HEADER_ARRAY.search(text)
    if not flag or not array:
        raise ValueError("not a generated ota_public_key.h")
    values = [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", array.group(1))]
    if len(values) != PUBLIC_POINT_BYTES:
        raise ValueError("kOtaPublicKey has %d bytes, expected %d" % (len(values), PUBLIC_POINT_BYTES))
    return flag.group(1) == "true", bytes(values)


def read_public_key_header(path=None):
    """parse_public_key_header() of `path`, by default include/ota_public_key.h."""
    return parse_public_key_header(Path(path or PUBLIC_KEY_HEADER).read_text(encoding="utf-8"))


def is_inside(path, directory):
    """Whether `path` (resolved) is `directory` or lies under it."""
    path = Path(path).resolve()
    directory = Path(directory).resolve()
    return path == directory or directory in path.parents
