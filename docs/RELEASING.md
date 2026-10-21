# Releasing

How the maintainer publishes a firmware release. Radars poll GitHub Pages and
install it by themselves ([OTA.md](OTA.md)); the web installer on the same
site uses it for new and repaired radars.

**A release is a tag.** Pushing `vMAJOR.MINOR.PATCH` starts the *Release*
workflow (`.github/workflows/release.yml`), which tests and builds that commit,
signs the image with the `OTA_SIGNING_KEY` repository secret, commits the signed
`site/` to `main` and deploys it to Pages. There is no step in between for a
person. Nothing there publishes an image with the airline logos:
`make_release.py` refuses one.

**The signing key is a repository secret.** Only the Release workflow reads it,
and it deletes the key file as soon as it has signed. But anyone who can push
to this repository can also change that workflow, so write access to the
repository is signing power over every radar: keep collaborators few, and do
not merge workflow changes you have not read.

---

## Once

1. **Signing key.** Make the pair on your own computer:

   ```bash
   mkdir -m 700 -p ~/.config/plane-radar
   python3 scripts/ota_keygen.py ~/.plane-radar/ota-signing-key.pem
   ```

   It asks for a passphrase, writes the private key there (mode 600; refused
   inside this or any other git checkout) and the public key into
   `include/ota_public_key.h`. Commit that header. (`python3` here means a venv
   with `pip install -r scripts/requirements-release.txt`.)

   **Back the private key up** (password manager or an offline copy) together
   with its passphrase. Lose it and the next image can only go onto radars
   over USB. If it leaks, anyone who can also change the Pages site could ship
   firmware to every radar: generate a new key, build, and re-flash radars
   over USB with an image that embeds the new one.

2. **The secret.** Repository *Settings → Secrets and variables → Actions →
   `OTA_SIGNING_KEY`*, holding the whole PEM file. It has to be **unencrypted**:
   CI cannot type a passphrase, and the workflow refuses an encrypted key. For a
   key that has no passphrase, `gh secret set OTA_SIGNING_KEY < key.pem` sets
   it without showing it; for an encrypted one, set it from the output of
   `openssl pkcs8 -in key.pem` (asks for the passphrase) in the same way.
   A missing or wrong secret fails the Release run in its first seconds, with
   the reason in the log.

3. **Pages.** *Settings → Pages → Source: GitHub Actions*, and in *Settings →
   Environments → github-pages → Deployment branches and tags* allow the tag
   pattern `v*` next to `main`. Without it, a tag-triggered run is refused at
   the Deploy job ("Tag … is not allowed to deploy to github-pages"). The
   release commit is on `main` by then; fix the setting and re-run the failed
   job, or run the *Pages* workflow.

4. If you protect `main`, let `github-actions[bot]` push to it: the Release
   workflow commits `site/` there directly.

## Each release

The image has to be logo-free (the logos are trademarks), fit an app slot,
use the two-slot layout, embed the real public key, and be built at a clean
release tag. `make_release.py` checks all of it and writes nothing otherwise;
the workflow runs it too.

1. **Pick the version.** `vMAJOR.MINOR.PATCH`, higher than the one in
   `site/ota/manifest.json`. Radars install only a strictly newer release.

2. **Bump `CHANGELOG.md`** (a `## vX.Y.Z` section) and commit it to `main`, with
   *Build* and *Quality* green on that commit.

3. **Tag and push**, from a clean checkout of `main`:

   ```bash
   git status                  # nothing modified
   git tag -a v2.1.0 -m "Larger labels"
   git push public main v2.1.0
   ```

   The first line of the tag message is the note radars show with the update
   (one line, 200 characters at most; a tag without a message gets "Release
   v2.1.0").

4. **Wait for the *Release* workflow** (Actions tab, a few minutes). It refuses a
   tag that is not `vMAJOR.MINOR.PATCH`, is not on `main`, or is not newer than
   what `main` publishes. Then it runs the host tests, builds `supermini`,
   checks that the image was built as exactly this tag, signs it, verifies the
   result the way a radar will (`make_release.py --verify-site`), commits
   `site/` to `main` as "Release v2.1.0" by `github-actions[bot]`, and deploys
   Pages. `git pull` before your next commit: `main` has moved.

5. **Confirm.** Open
   `https://ericcheng0904-pixel.github.io/plane-radar/ota/manifest.json` and
   check the version. The installer page shows it too. Pages serves it within
   minutes, and radars pick it up within about a day.

**If the run fails.** The log says why. Nothing reaches `main` before the
*Put site/ on main* step and nothing reaches Pages before the Deploy job. Fix
the cause, then run *Release* from the Actions tab with the same tag (it builds
the tag again), or, when the tag itself is wrong, delete it and tag again. A
version that `main` already publishes is refused: it is never re-signed. If only
the Deploy job failed, re-run that job, or run the *Pages* workflow, which
deploys what is on `main`.

## Signing locally (only if you hold a copy of the key)

CI is the normal way. If the Release workflow cannot run and you hold the
private key, the old way still works from a clean checkout at the tag:

```bash
pio run -e supermini            # the log must say: Plane Radar version: v2.1.0
python3 scripts/make_release.py --version v2.1.0 \
    --key ~/.plane-radar/ota-signing-key.pem --notes "Larger labels"
git add site && git commit -m "Release v2.1.0"
git push public main v2.1.0
```

*Pages* deploys the push to `main`. The Release run that the tag starts stops
at its first checks, because `main` already publishes that version.

## When a feed changes

The radar depends on six outside things: the aircraft feeds opendata.adsb.fi
and api.adsb.lol, the route lookup at adsb.im, Open-Meteo's town search, the
update manifest and image on GitHub Pages, and the certificate chains of all of
them. The *API watch* workflow asks each one every day the way the radar does
(`tools/api_watch.py`, at most two requests per service) and opens an issue
**API watch: <service> needs attention** when one answers with another status
or shape, or not at all. It adds a comment to that issue while the problem
lasts and closes it when the service is fine again. Run it any time from
Actions → *API watch*, or on your own computer:

```bash
python3 tools/api_watch.py              # all of them; also writes api-watch-report.md
python3 tools/api_watch.py adsb.lol     # just this one
```

*unknown from runner* in its table is not a fault: the service refused GitHub's
datacenter address (403, 429, or a challenge page), which a radar at home does
not share. It opens no issue; run the command above at home to see how the
service answers you.

What to change when it fires:

| Service | Where the radar uses it |
|---------|-------------------------|
| adsb.fi, adsb.lol | `kFeeds` in `src/services/adsb_client.cpp` (the radar fails over to the other feed meanwhile) |
| adsb.im routes | `src/services/routes.cpp` (without it the card shows no route) |
| Open-Meteo geocoding | `src/services/geocode.cpp` (town and ZIP search on the settings page) |
| GitHub Pages | `site/`; fix it and push, *Pages* deploys it. Radars cannot update meanwhile and keep working |
| Certificates | `data/cert/` via `scripts/build_cert_bundle.py`, then release: a radar stops checking a server's certificate after six rejections in a row |

Fix, host-test, and release a patch version as above. GitHub switches off
scheduled workflows (this one and *Certificate check*) in a repository with no
activity for 60 days; if that happens, re-enable them from the Actions tab.

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
