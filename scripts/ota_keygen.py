#!/usr/bin/env python3
"""
Generate the key pair that signs over-the-air updates. Run once, by the
maintainer, on their own computer (docs/RELEASING.md):

    python3 scripts/ota_keygen.py ~/.config/plane-radar/ota-signing-key.pem

  - The PRIVATE key goes to the path given, readable by you alone (0600). It
    is refused anywhere inside this repository or any other git checkout, and
    an existing file is never overwritten. You are asked for a passphrase to
    encrypt it; --no-passphrase skips that (tests, throwaway keys).
  - The PUBLIC key goes into include/ota_public_key.h, which is committed: the
    firmware embeds it and checks every update against it.

Keep the private key, and a backup of it, somewhere safe. Lose it and radars
can only be updated over USB again; leak it and anyone who can also change
what the update site serves could install their own firmware.

Replacing a real key (--replace-key) strands every radar running an image
built with the old one: they will refuse updates signed with the new key.

Needs the 'cryptography' package (scripts/requirements-release.txt).
"""

import argparse
import getpass
import os
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
import ota_common  # noqa: E402

from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import ec  # noqa: E402


def fail(message):
    sys.stderr.write("ota_keygen: %s\n" % message)
    sys.exit(1)


def inside_git_checkout(directory):
    try:
        result = subprocess.run(["git", "-C", str(directory), "rev-parse", "--show-toplevel"],
                                capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def check_key_path(path):
    path = Path(path).expanduser().absolute()
    if ota_common.is_inside(path, ota_common.REPO_ROOT):
        fail("refusing to write the private key inside this repository (%s).\n"
             "Choose a path outside it, e.g. ~/.config/plane-radar/ota-signing-key.pem" % ota_common.REPO_ROOT)
    parent = path.parent
    if not parent.is_dir():
        fail("directory %s does not exist; create it first (mkdir -m 700 ...)" % parent)
    checkout = inside_git_checkout(parent)
    if checkout:
        fail("refusing to write the private key inside a git checkout (%s): it could be committed." % checkout)
    if path.exists() or path.is_symlink():
        fail("%s already exists. A signing key is never overwritten; move it away yourself "
             "if you really mean to replace it." % path)
    return path


def ask_passphrase():
    if not sys.stdin.isatty():
        fail("no terminal to ask for a passphrase; run interactively or pass --no-passphrase")
    first = getpass.getpass("Passphrase for the private key: ")
    if len(first) < 8:
        fail("use a passphrase of at least 8 characters, or --no-passphrase")
    if getpass.getpass("Again: ") != first:
        fail("passphrases differ")
    return first.encode("utf-8")


def write_private_key(path, key, passphrase):
    encryption = (serialization.BestAvailableEncryption(passphrase) if passphrase
                  else serialization.NoEncryption())
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, encryption)
    # O_EXCL: never follow or replace something that appeared since the check;
    # mode 0600 from the start, so the key is never readable by others.
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as fp:
        fp.write(pem)
    os.chmod(str(path), 0o600)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("private_key", help="where to write the private key (outside any repository)")
    parser.add_argument("--header", default=str(ota_common.PUBLIC_KEY_HEADER),
                        help="public key header to write (default: include/ota_public_key.h)")
    parser.add_argument("--no-passphrase", action="store_true",
                        help="store the private key unencrypted (throwaway keys only)")
    parser.add_argument("--replace-key", action="store_true",
                        help="overwrite a header that already holds a real key")
    args = parser.parse_args(argv)

    key_path = check_key_path(args.private_key)
    header = Path(args.header)
    if header.exists():
        try:
            placeholder, point = ota_common.read_public_key_header(header)
        except ValueError:
            fail("%s exists but is not a generated header; not touching it" % header)
        if not placeholder and not args.replace_key:
            fail("%s already holds key %s. Radars built with it would refuse updates signed with a "
                 "new key; pass --replace-key only if you accept that." % (header, ota_common.fingerprint(point)))

    passphrase = None if args.no_passphrase else ask_passphrase()
    key = ec.generate_private_key(ec.SECP256R1())
    point = key.public_key().public_bytes(serialization.Encoding.X962,
                                          serialization.PublicFormat.UncompressedPoint)
    write_private_key(key_path, key, passphrase)
    header.write_text(ota_common.render_public_key_header(point), encoding="utf-8")

    print("Private key: %s (mode 600%s)" % (key_path, ", encrypted" if passphrase else ", NOT encrypted"))
    print("Public key:  %s" % header)
    print("Fingerprint: %s" % ota_common.fingerprint(point))
    print("Back up the private key now. Then commit %s." % header.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
