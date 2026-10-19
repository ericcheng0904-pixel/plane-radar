#!/usr/bin/env python3
"""
Check that every HTTPS server the radar talks to passes the radar's own
certificate check against data/cert/x509_crt_bundle.bin.

The check in Arduino-ESP32 2.0.x (esp_crt_bundle.c) looks at one certificate
only: the last one the server sends. Its issuer's name is looked up in the
bundle, and that root's key must verify its signature. Expiry dates are not
checked (the radar has no clock for it). So what breaks a radar is a server
that starts ending its chain in a root the bundle does not have; this script
connects to each server, takes the chain as sent, and repeats that lookup.

    python3 scripts/check_hosts_against_bundle.py            # all hosts
    python3 scripts/check_hosts_against_bundle.py adsb.im    # just these

Exits 1 if any host fails. Needs `openssl` on the PATH and the 'cryptography'
package. Run monthly by .github/workflows/cert-check.yml.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_cert_bundle import OUT, read_bundle  # noqa: E402

HOSTS = [
    "opendata.adsb.fi",              # aircraft, first choice
    "api.adsb.lol",                  # aircraft, reserve
    "adsb.im",                       # routes (plain HTTP today; HTTPS if redirected)
    "geocoding-api.open-meteo.com",  # town and ZIP code search
    "ericcheng0904-pixel.github.io",  # updates (GitHub Pages)
    "github.com",
]


def presented_chain(host: str) -> list[x509.Certificate]:
    out = subprocess.run(
        ["openssl", "s_client", "-connect", f"{host}:443", "-servername", host, "-showcerts"],
        input=b"", capture_output=True, timeout=30).stdout
    return x509.load_pem_x509_certificates(out) if b"BEGIN CERTIFICATE" in out else []


def verifies(key_der: bytes, cert: x509.Certificate) -> bool:
    key = serialization.load_der_public_key(key_der)
    try:
        if isinstance(key, rsa.RSAPublicKey):
            key.verify(cert.signature, cert.tbs_certificate_bytes, padding.PKCS1v15(),
                       cert.signature_hash_algorithm)
        elif isinstance(key, ec.EllipticCurvePublicKey):
            key.verify(cert.signature, cert.tbs_certificate_bytes,
                       ec.ECDSA(cert.signature_hash_algorithm))
        else:
            return False
    except Exception:  # noqa: BLE001 - any failure means the radar would refuse it
        return False
    return True


def check(host: str, bundle: dict[bytes, list[bytes]]) -> str | None:
    """None if the radar would accept `host`, else why not."""
    chain = presented_chain(host)
    if not chain:
        return "no certificate chain (could not connect?)"
    top = chain[-1]
    issuer = top.issuer.public_bytes()
    keys = bundle.get(issuer)
    if not keys:
        return f"chain ends in {top.subject.rfc4514_string()}, issued by " \
               f"{top.issuer.rfc4514_string()}, which is not in the bundle"
    if not any(verifies(k, top) for k in keys):
        return f"{top.issuer.rfc4514_string()} is in the bundle, but its key does not " \
               f"verify {top.subject.rfc4514_string()}"
    return None


def main(argv: list[str]) -> int:
    bundle: dict[bytes, list[bytes]] = {}
    for name, key in read_bundle(OUT.read_bytes()):
        bundle.setdefault(name, []).append(key)
    failed = 0
    for host in argv[1:] or HOSTS:
        why = check(host, bundle)
        print(f"{'ok  ' if why is None else 'FAIL'} {host}" + ("" if why is None else f": {why}"))
        failed += why is not None
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
