# Releasing

How the maintainer publishes a firmware release. Radars poll GitHub Pages and
install it by themselves ([OTA.md](OTA.md)); the web installer on the same
site uses it for new and repaired radars.

**The signing key never leaves the maintainer's computer.** Releases are
signed locally with `scripts/make_release.py`; CI only builds, checks and
deploys what is committed. Nothing in `.github/workflows/` can sign, and
nothing there publishes an image with the airline logos.

---

## Once

1. **Tools.** PlatformIO 6.2.0, and for the release scripts:

   ```bash
   python3 -m venv ~/.venvs/plane-radar-release
   ~/.venvs/plane-radar-release/bin/pip install -r scripts/requirements-release.txt
   ```

   (`python3` below means that venv's Python.)

2. **Signing key.**

   ```bash
   mkdir -m 700 -p ~/.config/plane-radar
   python3 scripts/ota_keygen.py ~/.plane-radar/ota-signing-key.pem
   ```

   It asks for a passphrase, writes the private key there (mode 600; refused
   inside this or any other git checkout) and the public key into
   `include/ota_public_key.h`. Commit that header.

   **Back the private key up** (password manager or an offline copy) together
   with its passphrase. Lose it and the next image can only go onto radars
   over USB. If it leaks, anyone who can also change the Pages site could ship
   firmware to every radar: generate a new key, build, and re-flash radars
   over USB with an image that embeds the new one.

3. **Pages.** Repository *Settings → Pages → Source: GitHub Actions*. The
   *Pages* workflow then publishes `site/` from `main`.

## Each release

The image has to be logo-free (the logos are trademarks), fit an app slot,
use the two-slot layout, embed the real public key, and be built at a clean
release tag. `make_release.py` checks all of it and writes nothing otherwise.

1. **Pick the version.** `vMAJOR.MINOR.PATCH`, higher than the one in
   `site/ota/manifest.json`. Radars install only a strictly newer release.

2. **Tag** the commit on `main` you are releasing, from a clean checkout:

   ```bash
   git status                  # nothing modified
   git tag -a v2.1.0 -m "v2.1.0: larger labels"
   ```

3. **Build** it:

   ```bash
   pio run -e supermini
   ```

   The log must say `Plane Radar version: v2.1.0` — not `-dirty`, not
   `-3-g…` — and the size guard must pass.

4. **Sign and lay out the site:**

   ```bash
   python3 scripts/make_release.py --version v2.1.0 \
       --key ~/.plane-radar/ota-signing-key.pem \
       --notes "Larger labels"
   ```

   It refuses, and says why, if anything is off. Otherwise it writes
   `site/ota/manifest.json`, `site/ota/firmware-v2.1.0.bin` (removing the
   previous image), the installer's `site/manifest.json` and `site/flash/`.

5. **Publish:**

   ```bash
   git add site
   git commit -m "Release v2.1.0"
   git push origin main v2.1.0
   ```

   *Pages* checks the site the way a radar will (`make_release.py
   --verify-site`) and deploys it. *Release check* builds the tag and runs the
   same checks on it.

6. **Confirm.** Open
   `https://ericcheng0904-pixel.github.io/plane-radar/ota/manifest.json` and
   check the version. The installer page shows it too. Radars pick it up
   within about a day.

## When a release is bad

- A radar that cannot boot the new image, or cannot get back on Wi-Fi and
  fetch aircraft with it within 10 minutes, rolls back to the previous image
  by itself.
- Do not delete or re-sign a published version. Radars never go back down, so
  fix forward: release `v2.1.1`.

## Before the updater exists

Until the firmware has an updater, `include/ota_public_key.h` may still be the
placeholder and images do not embed the key, so `make_release.py` refuses to
publish. `--allow-missing-ota-key` gets past the second check (the first needs
a real key). Use it only knowingly: every radar installed from that release can
**never update itself** and will need USB for the next version.

## Airline logos

No image contains logos, so a release is never affected by them. A radar's
owner adds them once over USB, from a private pack (needs Pillow):

```bash
python3 scripts/build_airline_brands.py --logos     # writes .cache/logo-pack.bin
esptool.py --chip esp32c3 write_flash 0x390000 .cache/logo-pack.bin
```

The pack goes into the `logos` partition of the normal two-slot layout. Updates
rewrite only an app slot, so they never remove it. Never publish the pack —
`scripts/check_no_logos.py` fails CI if it is tracked.
