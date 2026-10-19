# Quality pipeline

Five stages, cheapest first. Stages 1–4 run in CI on every pull request; stage 5
is a human checklist, because nothing in CI can see the panel.

The ordering is deliberate: a class of defect caught in stage 2 costs seconds,
the same defect caught in stage 5 costs a flash cycle and a pair of eyes.

## Stage 1 — Compiler warnings are errors (project code)

`platformio.ini` builds with `-Wall -Wextra`. The **Build** workflow greps the
build log and fails on any `warning:` or `error:` whose path is under `src/` or
`include/`. Third-party libraries warn plenty and are not ours to fix, so they
are deliberately not matched.

Locally: `pio run -e supermini` and read the output.

## Stage 2 — Host unit tests

`test/test_util/` covers the parsers that sit behind the Wi-Fi setup portal —
`util::portal::checkboxChecked`, `util::portal::stepIndex`,
`util::coords::parseCoordinate`, `util::coords::validLatLon`. `test/test_setup/`
covers the guided setup: pasted coordinate pairs and map links, the town / ZIP
code query and the ranking of the geocoder's answers, the Wi-Fi QR text, join
failure reasons, HTML escaping, and the self-repair rules setup changes. They
run on the build machine, so every malformed and hostile field value can be
exercised without a board, a network, or a flash cycle.

```bash
pio test -e native
```

`test/test_ota/` covers the rules the over-the-air updater will follow: which
version strings are releases, that only a strictly newer release is installed,
and that the embedded public key is the placeholder or a well-formed point.
The release tooling has Python tests of its own in `tools/ota/`: they sign a
synthetic build with a throwaway key, verify it the way a radar will, and check
every refusal (logos, size, layout, version, key).

```bash
pip install -r scripts/requirements-release.txt
python3 -m unittest discover -s tools/ota -v
```

Anything reachable from the portal or from the network belongs here. Keep such
logic free of Arduino headers (`include/util/`, `src/util/`) so it stays
testable; the rule of thumb is that a parser or a piece of geometry that needs
`WiFi.h` to compile is in the wrong file.

### The settings page renders whole

The settings page is the one piece of portal code that cannot be made free of
Arduino headers, so `tools/host_checks/` supplies just enough of `WebServer` and
`String` to render it on the build machine and assert that every control, the
Save button and the closing tag are there.

```bash
g++ -std=gnu++17 -Wall -Wextra -Werror -I include -I tools/host_checks/stubs \
    tools/host_checks/portal_page_test.cpp src/services/portal_pages.cpp \
    src/util/coords.cpp src/util/setup_text.cpp \
    -o /tmp/portal_page_test && /tmp/portal_page_test
```

This exists because of a defect that no compiler and no static analyser could
see: WiFiManager assembled the page as one Arduino `String`, and `String` gives
no error when the heap cannot grow it. The page stopped after six controls, with
no Save button and nothing in the log. The page is now streamed in small chunks;
the check fails the build if it ever stops short again.

## Stage 3 — Static analysis

`cppcheck` over `src/` and `include/` with `--enable=warning,performance,portability`,
failing the build on any finding. Third-party headers are not checked out in
that job, so unresolved includes are suppressed; everything cppcheck can still
prove about this project's own code has to come back clean.

A finding that is genuinely a false positive gets an inline
`// cppcheck-suppress <id>` with a comment saying why — never a blanket
suppression in the workflow.

### No airline logos in the repository

Airline logos are the airlines' trademarks. They live in a private logo pack,
the git-ignored `.cache/logo-pack.bin` (`scripts/build_airline_brands.py --logos`),
flashed once over USB into the radar's `logos` partition
(`esptool.py --chip esp32c3 write_flash 0x390000 .cache/logo-pack.bin`); the
repository, and therefore every release and OTA image, must stay logo-free.
`scripts/check_no_logos.py` fails on logo-named arrays, a file full of
RGB565-style hex data, the pack or a `*.local.h` logo file being tracked, or any
tracked file over 1 MB.

```bash
python3 scripts/check_no_logos.py
```

## Stage 4 — Firmware build and artifact

`pio run -e supermini`, then `pio run -t merge` for the web-flashable image.
Fails on link errors and when `firmware.bin` outgrows an app slot
(`scripts/size_guard.py`: over `0x1C0000` fails, over 90 % warns). Before
`plane-radar-supermini` (including `firmware-merged.bin`) is published for
download, `make_release.py --check-only` refuses an image with the airline
logos or a partition table other than the two-slot release layout.

Watch the reported RAM figure. It excludes the 115 KB frame sprite, which is
allocated at runtime — headroom is tighter than the percentage suggests.

## Stage 5 — Hardware smoke test

CI cannot see the display, reach a Wi-Fi network, or press BOOT. Before merging
anything that touches the UI, the portal, or the network path, flash the build
and walk this list:

- [ ] Boot with no stored credentials → "Step 1 of 2" with a QR code, fully
      inside the glass. A phone camera joins `PlaneRadar-Setup` from it; a tap
      on BOOT shows the network name and password instead, another tap the
      code again. With a phone on the hotspot the screen says "Phone connected".
- [ ] The hotspot page has the "Step 1 of 2" header, "Network" for SSID, and
      no navigation bar. A wrong password → "Could not join / Check the
      password" within ~20 s, then step 1 again; the radar still has no saved
      network afterwards (power-cycle: step 1, not "Can't reach Wi-Fi").
- [ ] The right password → "Step 2 of 2" with a QR code for
      `http://<ip>/settings`. Saving a town ("Evanston IL") or a ZIP code
      shows "Location saved / Showing planes near …" for 3 s, then the radar.
      Power-cycle → the radar comes straight back (the Wi-Fi was kept in flash).
- [ ] Portal reachable again at `http://plane-radar.local` once on the LAN, and
      its menu offers no **Update** (OTA) entry.
- [ ] The navigation bar (Home / Wi-Fi / Settings / System) appears on all four
      pages, the current page is highlighted, and every link works. If a phone
      blocks scripts the bar is absent — the pages still work through the Home
      buttons.
- [ ] The Settings page shows the four group headings and every option; saving
      from there applies immediately without touching the Wi-Fi credentials.
- [ ] `http://<device-ip>/update`, `/u`, `/erase` and `/restart` each return
      404 — these are the routes WiFiManager would otherwise expose unauthenticated.
- [ ] Change **Text size** to 1, 2 and 3 → labels resize immediately, no reboot,
      nothing clipped at the round edge, aircraft tags do not collide.
- [ ] Untick **Show aircraft direction lines** → lines disappear, triangles stay.
      Re-tick → they come back.
- [ ] Range presets still cycle on a short BOOT tap, and the range label matches.
- [ ] Hold BOOT 10 s (countdown after 2 s) → only the Wi-Fi is cleared; the
      radar reboots into step 1 and, once rejoined, keeps its location.
- [ ] Settings → **Reset for a new owner** → confirm page → location, display
      settings and Wi-Fi erased, reboot into "Step 1 of 2".
- [ ] Untick **Show flight trails** → tails disappear, symbols stay. Re-tick and
      wait two fetch intervals → tails grow again and follow the aircraft.
- [ ] Untick **Separate symbols…** → every aircraft is the original triangle.
      Re-tick → helicopters show as rotor discs, wide-bodies as large triangles.
- [ ] Set **Alert flash seconds** to 3, then wait for a military or wide-body
      contact → the ring pulses for about three seconds and the radar returns
      to normal. The same aircraft must not fire again on the next sweep.
- [ ] Set it to 0 → no flash at all.
- [ ] An airline flight shows its operator's name on the top tag line; a
      registration or hex id still shows verbatim.
- [ ] On a radar with no logo pack the info card shows an airline flight's code on a
      badge in its brand colour ("WN" on blue for Southwest, "DL" on red for
      Delta), the code centred and readable; a light aircraft or helicopter
      still gets its silhouette badge.
- [ ] Serial log reports the CPU clock at boot (160 MHz) and a chip
      temperature once a minute.
- [ ] The **Settings** page renders every group down to the **Save** button,
      and Save reports success. The **System** page offers no Update button and
      no list of pages the firmware refuses to serve.
- [ ] Raise **Wi-Fi transmit power** to 19 → the log confirms it and the device
      stays connected. Drop it back to 8 if the board browns out or reboots.
- [ ] Pull the plug on the router for ~10 s → the radar picture stays up and
      recovers by itself. Only an outage past ~25 s brings up "Wi-Fi lost";
      after 10 min the radar restarts and opens the hotspot between retries.
- [ ] Flash a build with the two-slot layout over a radar that had the old one,
      without erasing (`pio run -t upload`, or the web installer with *Erase*
      unticked) → it boots, and Wi-Fi, location and settings are all still
      there.
- [ ] Leave it running ~15 minutes → aircraft keep updating, no reboot loop
      (watch the serial log at 115200 baud for stack traces).

Record the outcome in the pull request. A stage-5 failure means the change is
not ready, however green CI is.
