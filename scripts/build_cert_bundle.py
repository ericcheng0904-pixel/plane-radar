#!/usr/bin/env python3
"""
Build data/cert/x509_crt_bundle.bin: the root certificates the radar trusts
for HTTPS, in the format WiFiClientSecure::setCACertBundle() reads (the same
as ESP-IDF's gen_crt_bundle.py).

Format: a big-endian u16 certificate count, then for each root, sorted by its
DER subject name (the device binary-searches on it):
    u16 subject length, u16 public key length, subject DER, SubjectPublicKeyInfo DER

The roots are Mozilla's list as published by curl (https://curl.se/ca/cacert.pem),
plus data/cert/extra_roots.pem (roots the device still needs for cross-signed
chains; see that file), plus every root the committed bundle already has: the
bundle only grows. A radar in the field cannot be told that a root left
Mozilla's list, and the servers it talks to may still send chains that end
in one, so a root is dropped only by listing it in DROP below, with a reason.
Only names and public keys are kept, so the bundle is ~1/3 of the PEM's size.

scripts/check_hosts_against_bundle.py checks the servers against the result.

    python3 scripts/build_cert_bundle.py               # download the current list
    python3 scripts/build_cert_bundle.py cacert.pem    # or use a local copy

Needs the 'cryptography' package.
"""

import struct
import sys
import urllib.request
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import serialization

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "data" / "cert" / "x509_crt_bundle.bin"
EXTRA = ROOT / "data" / "cert" / "extra_roots.pem"

# Roots to remove from the bundle on purpose (e.g. a distrusted CA), as
# substrings of the DER subject, each with the reason. Empty so far.
DROP: dict[bytes, str] = {}


def read_bundle(data: bytes) -> list[tuple[bytes, bytes]]:
    """(subject, public key) pairs of a bundle in the device's format."""
    count = struct.unpack(">H", data[:2])[0]
    entries, off = [], 2
    for _ in range(count):
        name_len, key_len = struct.unpack(">HH", data[off:off + 4])
        off += 4
        entries.append((data[off:off + name_len], data[off + name_len:off + name_len + key_len]))
        off += name_len + key_len
    return entries
SOURCE_URL = "https://curl.se/ca/cacert.pem"


def load_pem(argv):
    if len(argv) > 1:
        return Path(argv[1]).read_bytes()
    req = urllib.request.Request(SOURCE_URL, headers={"User-Agent": "PlaneRadar-build"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return resp.read()


def main(argv):
    pem = load_pem(argv)
    certs = x509.load_pem_x509_certificates(pem)
    extra = x509.load_pem_x509_certificates(EXTRA.read_bytes())
    certs += extra
    entries = {}
    for cert in certs:
        name = cert.subject.public_bytes()
        key = cert.public_key().public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
        # Same subject, same key: a re-issued root. Keep one.
        entries.setdefault((name, key), None)
    kept = 0
    if OUT.exists():
        for entry in read_bundle(OUT.read_bytes()):
            if entry not in entries:
                kept += 1
            entries.setdefault(entry, None)
    for name, key in list(entries):
        for needle, why in DROP.items():
            if needle in name:
                print(f"dropping a root ({why})")
                del entries[(name, key)]
                break
    items = sorted(entries.keys(), key=lambda nk: nk[0])
    out = bytearray(struct.pack(">H", len(items)))
    for name, key in items:
        out += struct.pack(">HH", len(name), len(key)) + name + key
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(bytes(out))
    print(f"{OUT.relative_to(ROOT)}: {len(items)} roots ({len(extra)} extra, "
          f"{kept} kept from the previous bundle), {len(out)} bytes")


if __name__ == "__main__":
    main(sys.argv)
