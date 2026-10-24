# Plane Radar

A live aircraft radar on a 1.28″ round display (ESP32-C3): this version is
extended firmware with a browser installer, phone setup and over-the-air
updates, built on the hardware and case from
[MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar)
(MIT licence, kept in [LICENSE](LICENSE)) and the firmware of the
[fabsface-max/ESP32-Plane-Radar](https://github.com/fabsface-max/ESP32-Plane-Radar)
fork.

<!-- hero: replace with media/hero.gif (owner clip) -->

<img width="800" height="450" alt="plane-radar" src="https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419" />

**[Install in your browser](https://ericcheng0904-pixel.github.io/plane-radar/)**
(Chrome or Edge).

Were you given one? [Start here.](docs/GIFT_GUIDE.md)

## What you need

- An ESP32-C3 Super Mini
- A 1.28″ round GC9A01 display, wired as in [Wiring](#wiring)
- A USB-C cable and a 2.4 GHz Wi-Fi network
- The 3D case on [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083)

## What's different from upstream

| | [MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar) | [fabsface-max fork](https://github.com/fabsface-max/ESP32-Plane-Radar) | This version |
|---|---|---|---|
| Picture | One font, scaled down. Aircraft drawn as one triangle. | Native fonts at three sizes, airline names, a symbol per class, flight trails, direction lines that can be switched off, and an alert flash. | Info card, shapes by kind, flight-number tags, four themes, night dimming, and a Chicago-area water layer. |
| Setup | Options sat under the Wi-Fi form, and the page could be cut off. The setup Wi-Fi was open. | Its own settings page, written in pieces, with navigation. The open setup Wi-Fi and the unauthenticated upload page are closed, transmit power is adjustable, and a short Wi-Fi drop leaves the picture up. | Phone setup with a QR code for each step, a welcome screen, and an info screen. |
| Aircraft data | Open report: fetches fail with a JSON parse error ([#85](https://github.com/MatixYo/ESP32-Plane-Radar/issues/85)). | adsb.fi. | adsb.fi over HTTPS, with adsb.lol in reserve. Replies are stream-parsed, chunked bodies included, and the frame is drawn in two halves so a TLS handshake fits. |
| Updates | Open issue: the portal Update page cannot install firmware, because there is no second app slot ([#102](https://github.com/MatixYo/ESP32-Plane-Radar/issues/102)). | The upload page is closed. A new image is flashed over USB. | Two app slots and signed over-the-air updates, plus the browser installer above. The upload page stays closed. |
| Refresh | Open report: the screen stops refreshing until a reboot ([#109](https://github.com/MatixYo/ESP32-Plane-Radar/issues/109)). | A short Wi-Fi drop leaves the radar on screen. | Background fetch and a steady redraw (upstream pull requests #90 and #94). The radar restarts itself if the display loop stalls. |
| Checks | — | Compiler warnings, host tests, static analysis, a firmware build and a hardware checklist, in CI. | The same pipeline. |

The rows above are the short version of [What the fabsface-max fork added](#what-the-fabsface-max-fork-added) and [What this version adds](#what-this-version-adds).

---

## What the fabsface-max fork added

| | Change | Why it matters |
|---|---|---|
| 🔤 | **Sharp text at three sizes** | The original scaled one font down, which drops pixel rows and makes small labels ragged. Now four fonts are built into the firmware, one per size, so every label is drawn at its native size. Pick normal / small / smallest in the settings page. |
| ✈️ | **Airline names instead of codes** | `DLH4AB` now reads `Lufthansa`. ~5 800 airlines are built in. Registrations and hex ids are left alone. (This version shows the name on the info card and the flight number on the tag.) |
| 🚁 | **Symbols per aircraft class** | Helicopters get a rotor disc, wide-bodies a larger symbol. Switchable. (This version tells aircraft apart further, see below.) |
| 〰️ | **Flight trails** | Thin grey tail through each aircraft's last positions — roughly 20 seconds of history. Switchable. |
| 🚨 | **Alert flash** | The radar pulses a coloured ring when an emergency squawk, a military aircraft or a rare type (A380, 747, An-124…) first appears. 3, 5 or 7 seconds, or off. |
| 🔴 | **Direction lines can be switched off** | The line ahead of each aircraft is now optional. |
| 🔒 | **Three security holes closed** | An **unauthenticated firmware-upload page** was reachable from the whole local network, along with remote credential wipe and reboot. The setup Wi-Fi was **open**. A malicious server could **crash the device on demand** with an oversized reply. See [Security](#security). |
| 🌡️ | **Radio power you can trade** | A choice of Wi-Fi transmit power, for the trade-off between heat, range and stability. The chip temperature is on the System page. |
| 📶 | **Rides out short Wi-Fi drops** | A brief hiccup no longer replaces the radar with the search screen. |
| 🧭 | **A settings page you can find — and that finishes** | The options used to sit below the Wi-Fi credential form, with no navigation between pages. Now they have their own grouped page, served by this firmware rather than by WiFiManager, and every page carries the same navigation bar. WiFiManager built the page as one large string in RAM and cut it off without a word when memory ran short — half the options and the Save button simply never appeared. The page is now written out in small pieces, so its length no longer depends on free memory. |
| ✅ | **A five-stage quality pipeline** | Compiler warnings, host unit tests, static analysis, firmware build and a hardware checklist — all in CI. It has already caught real defects. See [docs/QUALITY.md](docs/QUALITY.md). |

Everything switchable lives in the device's own settings page — no reflashing,
no recompiling, and the changes take effect immediately.

---

## What this version adds

Built on top of the fork above, and checked on a real radar through the
`/screen.bmp` endpoint after every change.

| | Change |
|---|---|
| 🛫 | **Info card** — one nearby aircraft at a time: airline badge (its logo if the owner has flashed a logo pack, otherwise its code on its brand colour), name and flight number, aircraft type in words ("Boeing 737 MAX 8"), altitude with climb/descent arrow, speed. Rotates through the three nearest every 6 s, sits at the bottom or top (whichever hides less traffic), and marks its aircraft with lock brackets. |
| ✈️ | **Aircraft shapes by kind** — top-down silhouettes rotated to heading, coloured by altitude (orange low → violet cruise), with a dark outline: regional jet, narrow-body, wide-body, turboprop, business jet, light aircraft and helicopter, with the engine count where it is known. Switchable. See [Aircraft symbols](#aircraft-symbols). |
| 🏷️ | **Tags show the flight number** ("WN1966", IATA-style) instead of a truncated airline name, and are placed so they no longer overlap each other, aircraft or the card; a tag with no room keeps just its flight number. |
| 🌊 | **Water layer** — Lake Michigan from OpenStreetMap, tinted into the backdrop with an anti-aliased shoreline. It covers the Chicago area only; elsewhere the radar shows no water. The build script drops rivers, canals and ponds (opening), fills harbours and piers (closing) and simplifies to 50 m, so the shore reads as one clean line. |
| 🧭 | **Settings pages** — WiFiManager's pages pinned to the same light style as the settings page (they were black-on-black in dark mode), proper tab titles, and a tip for finding coordinates. |
| 🎛️ | **Scope restyle** — navy vignette, hairline rings, compass ticks every 10°; colours now look on the glass the way they are named (see below). |
| 🎨 | **Themes** (Settings → Theme): *Scope* (default), *Map* — deep blue with land as a halftone dot pattern, 12 spokes, double bezel, orange aircraft and a double-ring reticle, after the Kongji S1 "blue" skin — *Classic* green on black, and *Sweep*, a rotating beam whose afterglow lights the blips as it passes (redraws at 8 fps instead of 4). |
| 🌙 | **Night dimming** — the screen dims from dusk to dawn where the radar is. A setting. |
| 🔄 | **Upstream pipeline merged** — background fetch task and 4 Hz dead-reckoned redraw from MatixYo/ESP32-Plane-Radar pull requests #90 and #94. |
| 🌐 | **Aircraft over HTTPS, with a reserve** — opendata.adsb.fi (v3 API) first and api.adsb.lol as the reserve, with automatic failover. Certificates are checked against an embedded root bundle (`data/cert/`, built by `scripts/build_cert_bundle.py`; CI checks every server against it monthly with `scripts/check_hosts_against_bundle.py`). A TLS handshake does not fit in RAM next to a whole-frame buffer, so the frame is drawn in two 120-row halves. A feed that refuses (4xx) is rested for hours, adsb.lol is rationed to stay well inside its daily allowance (the first 60 requests of a day at the usual 10 s, then one every 4.5 min, 400 at most), and both are remembered across restarts. See [Where the data comes from](#where-the-data-comes-from). |
| ⚡ | **~60% less drawing work** — a frame took 141 ms (runways re-tested all 1,706 entries every frame, water shore was found pixel by pixel); now ~55 ms, which also means less heat. |
| 🧭 | **Origin and destination** — tags read flight number / route (`ORD-LAF`) / altitude, the card adds "SNA → DTW" (always IATA codes). From adsb.im's route API (it checks each route against the aircraft's position; implausible ones are not shown), at most one batch of 8 flights every 20 s, answers cached for hours. It starts on plain HTTP and follows a redirect to HTTPS by itself. Multi-stop flight numbers ("ORD-CHS-ORD") are cut to the leg being flown when the aircraft is clearly landing at or leaving a stop. With no trusted route, an airline flight (not a helicopter or light aircraft) low near a major airport and clearly descending toward it or climbing away gets one end: "→ ORD" / "ORD →" (util::airport_guess: below 10,000 ft, within 35 km, plausible height for the distance, track within 30°, nearest airport clearly nearest). |
| 🛟 | **Self-repair for unattended use** — restarts itself if the display loop stalls, fetching stops, no data arrives for 30 min, memory stays low or HTTPS keeps finding no room, and once a day; the reason is shown on the settings page. A saved Wi-Fi that is out of reach at power-up (router still starting) is retried every 3 min instead of leaving the radar in setup mode. "No flight data" appears on screen when it applies. |
| 📱 | **Guided setup from a phone** — two steps on the round screen, each with a QR code: join the setup hotspot and pick the home Wi-Fi (tried before it is kept; a wrong password or a 5 GHz-only network is named on screen), then open the settings page and give a town, ZIP code, pasted coordinates or a map link. "Wi-Fi lost" after 25 s, a restart into the hotspot path after 10 min, and **Reset for a new owner**. Afterwards a one-time welcome screen, by the radar's name if it has one, offers a short guide (`/guide`). See [If you were given one](docs/GIFT_GUIDE.md). |
| ℹ️ | **Info screen** — hold the button 2–9 s and let go: a QR code and the address of the settings page, the version and the data status. Holding 10 s forgets only the Wi-Fi. |
| 🔐 | **Signed over-the-air updates** — the flash has two 1.75 MB app slots. About once a day (and a few minutes after power-up) the radar looks for a release on GitHub Pages and installs it only if it is newer and its signature checks out against the key built into the firmware. The new image runs on trial and is kept only after it has joined Wi-Fi, drawn the radar, fetched aircraft and run for 15 minutes; otherwise the radar goes back to the old one by itself. Wi-Fi, location, settings and the logo pack are kept. See [docs/OTA.md](docs/OTA.md) and [docs/RELEASING.md](docs/RELEASING.md). |
| 🧰 | **Browser installer** — new and repaired radars can be flashed from a web page served from GitHub Pages (`site/`). See [Build and flash](#build-and-flash). |
| 📷 | **`/screen.bmp`** — the current frame as a BMP, for checking the display remotely. |

**Panel colours.** This GC9A01 shows the sprite's red and blue swapped. The
aircraft red was corrected for that upstream, but the background, grid and
runway colours were not and showed as brown, olive and khaki. All colours now go
through `ui::glassColor()`, and `/screen.bmp` swaps back so it matches the glass.

**No logos in this repository.** Airline logos are the airlines' trademarks, so
no firmware image contains them, including one built from a fresh clone. The
logos live in the radar's own flash partition, flashed once over USB from a
private pack that its owner builds, and updates never touch them. Without the
pack the card shows each airline's code on its brand colour ("UA" on United
blue). See [Airline logos](#airline-logos). **Never commit the pack or publish
it.** CI's `scripts/check_no_logos.py` fails if logo data reaches a tracked file.

---

## Build and flash

**You need:** an ESP32-C3 Super Mini, a 1.28″ round GC9A01 display, a USB-C
cable, and a 2.4 GHz Wi-Fi network. The wiring is at the end of this section;
the 3D case is on [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083).

### Flash from the browser

Open the installer at <https://ericcheng0904-pixel.github.io/plane-radar/> in
**Chrome or Edge** (Firefox and Safari cannot talk to USB devices), connect the
board and follow the page. It installs the newest published release. Leave
*Erase* unticked to keep the radar's settings. If the board does not appear, put
it into download mode: hold **BOOT**, tap **RESET**, release **BOOT**.

Then follow [If you were given one](docs/GIFT_GUIDE.md) for the setup.

### Build with PlatformIO

```bash
pio run -e supermini                  # build
pio run -e supermini -t upload        # build and flash over USB
pio device monitor                    # serial log, 115200 baud
pio test -e native                    # host unit tests
```

The build stamps the version from `git describe` (`v2.1.0` at a tag) and fails
if `firmware.bin` is larger than an app slot (`0x1C0000`). Settings live in
their own flash range, so `pio run -t upload` keeps them.

A single-file image (ESP32-C3, 4 MB, flash at `0x0`) is also built, for web
flashers. Writing it fills the settings range too, so it wipes the settings:

```bash
pio run -e supermini && pio run -t merge -e supermini
# -> .pio/build/supermini/firmware-merged.bin
```

A radar still on an older flash layout needs one USB flash to reach the
two-slot layout; an update over the air cannot change it
([docs/OTA.md](docs/OTA.md)).

### Airline logos

Optional, and private to your own radar. Build the pack (needs Pillow), then
flash it once into the radar's `logos` partition:

```bash
python3 scripts/build_airline_brands.py --logos      # writes .cache/logo-pack.bin
esptool.py --chip esp32c3 write_flash 0x390000 .cache/logo-pack.bin
```

The pack is built from FlightAware's airline logos; they are the airlines'
trademarks, and the collection they come from has no licence. The firmware
reads it from the partition at run time. Updates rewrite only an app
slot, so they never remove it; erasing the whole flash does, and then you flash
the pack again. Without a pack the card shows the airline's code.

### Releasing

Pushing a version tag (`vX.Y.Z`) makes CI build the firmware, sign it with a
key held as a repository secret, and publish it to GitHub Pages. The steps are in
[docs/RELEASING.md](docs/RELEASING.md) and the update rules in
[docs/OTA.md](docs/OTA.md). A radar installs only an image signed with the key
whose public half is in `include/ota_public_key.h`. If you fork the project,
make your own pair with `scripts/ota_keygen.py` and keep the private key off
the repository.

### Wiring

| Display | ESP32-C3 |
|---------|----------|
| VCC | 3V3 |
| GND | GND |
| RST | GPIO **0** |
| CS | GPIO **1** |
| DC | GPIO **10** |
| SDA (MOSI) | GPIO **3** |
| SCL (SCLK) | GPIO **4** |
| BOOT (user) | GPIO **9** |

---

## Settings reference

Everything below is on the **Settings** page at **`http://plane-radar.local`**
and is remembered across reboots. Five pages share one navigation bar:

| Page | What is on it |
|------|---------------|
| **Home** | Where you land; links to the rest |
| **Wi-Fi** | Which network the radar is on, and how to move it to another (hold the button 10 s) |
| **Settings** | Location, display, alerts, network & power — grouped; version and updates; **Reset for a new owner** at the bottom |
| **Guide** | What the radar is and how to read it, a short read for whoever was given one (`/guide`). Until it has been opened once, **Settings** points at it at the top |
| **System** | Chip details, memory, uptime, **temperature** |

| Setting | What it does |
|---------|--------------|
| **Location** | Where the radar is centred, and the position it asks the ADS-B service about: a town or ZIP code (the radar looks it up itself with Open-Meteo's geocoding; a ZIP code finds the middle of its town), pasted coordinates (`41.88, -87.63`, Apple Maps' `41.88° N, 87.63° W`, or a Google Maps link), or exact latitude/longitude. Its own form and **Save location** button; fields left empty change nothing |
| **Name of this radar** | Optional, up to 32 plain characters (no accents or emoji: the screen's font has none; a phone's curly quotes and dashes are made plain). The setup page, the welcome screen and the guide greet the owner by it; empty says "your Plane Radar" |
| **Text size** | `1` normal, `2` small, `3` smallest |
| **Theme** | Scope, Map, Classic or Sweep |
| **Distances in miles** | Ring labels in `mi` instead of `km` |
| **Show airport runways** | Runway overlay for major airports |
| **Aircraft shapes by kind** | Regional, narrow-body, wide-body, turboprop, business jet and engines, instead of one triangle |
| **Show direction lines** | The line ahead of each aircraft showing heading and speed |
| **Show flight trails** | Thin grey tail through recent positions |
| **Dim the screen at night** | Dusk to dawn where the radar is |
| **Flash duration** | `Off`, or `3` / `5` / `7` seconds — how long the radar pulses for a noteworthy aircraft |
| **Wi-Fi transmit power** | `8 dBm` low (default), `13` medium, `19` high — see [Heat and stability](#heat-and-stability) |
| **Check for an update now** | Looks for a release on the next pass; the result is under *About* |
| **Reset for a new owner** | Asks again, then erases the location, the name, the settings and the Wi-Fi, and restarts at step 1; with **Keep the location and the radar's name** ticked, at Wi-Fi setup. The welcome screen shows again for whoever sets it up next, so reset last |

Until a location is saved the radar shows nothing but step 2 of the setup — no
radar centred on a default somewhere else.

### The BOOT button

One tap (under 1.5 s) cycles the range (5 → 10 → 15 → 25 km) while the radar is
showing; on a setup or message screen it does not change the range, which could
not be seen there. On the welcome screen a tap skips the welcome. On the hotspot screen of step 1 a tap steps between the QR
code and the network name and password in words.

Holding for 2–9 s and letting go shows the info screen while the radar is up
(a tap closes it, or it closes itself after 20 s). **Holding for 10 seconds**
clears the saved Wi-Fi and returns to step 1; location and settings are kept. A
countdown appears after 2 s, so a hold can still be cancelled by letting go, and
presses in the first 5 s after power-up are ignored. (This was a silent 3 s hold
that also wiped location and settings, and handling the radar to swap its cable
was enough to trigger it.) The Wi-Fi page no longer takes new credentials:
saving there kept a mistyped password before it had been tried, cutting the
radar off the network the page was reached through.

### Wi-Fi trouble

A drop shorter than 25 s leaves the picture up. After that the screen says
*Wi-Fi lost* and keeps trying; after 10 minutes the radar restarts, which brings
up the setup hotspot between retries of the saved network — the way in after a
new router. During setup the radar tries a network before keeping it: a wrong
password shows *Could not join … Check the password*, an unseen network *Not
found … Use a 2.4 GHz network*, and after 20 s the QR code comes back.

---

## How the radar reads

### The picture

Rings and crosshairs on a dark background (the colours depend on the theme),
white **N / S / E / W** at the edge. The label on the east spoke gives the
distance of the third ring. Aircraft further away than the outer ring show as a
**dot on the rim**, in the right direction but not at the right distance.

### Aircraft symbols

A regional jet, an A320 and a 787 should not look alike on the glass. The
symbol follows the aircraft's kind, and carries its engine count where it is
known (on the rear fuselage for CRJ, ERJ, MD-80 and business jets):

| Kind | Chosen when |
|------|-------------|
| Light | emitter category `A1`: piston and light singles |
| Regional jet | CRJ, ERJ, E-Jets, ARJ21, Superjet |
| Narrow-body | A220 and A320 family, 737, 757, MD-80 |
| Wide-body | 767, 777, 787, A330, A350, 747, A380, MD-11 |
| Turboprop | Dash 8, ATR, King Air, C-130, PC-12 |
| Business jet | Citation, Learjet, Challenger, Gulfstream, Falcon |
| Heavy, kind unknown | category `A5` |
| Helicopter | rotor disc; category `A7`, or a rotorcraft type code (`EC35`, `R44`, …) |
| Jet | the original triangle: everything else, including unknown |

The feed is inconsistent — `category`, `squawk`, `emergency` and `dbFlags` are
all optional and most aircraft send none of them — so every rule falls back to
the plain jet triangle rather than guessing. Type tables are in
`src/util/aircraft_class.cpp` and match **exactly, never by prefix**: `B47` is a
Stratojet, `B47G` a Bell 47. With **Aircraft shapes by kind** off, every
aircraft is drawn as a plain triangle.

### Labels

Three lines beside each symbol: flight number (`WN1966`), route (`ORD-LAF`)
when one is known, altitude. A tag with no room keeps just its flight number.
The airline's name, the aircraft type in words and the speed are on the info
card.

### Alerts

When something noteworthy first appears, a coloured ring pulses outward from the
centre for the configured number of seconds. Each aircraft fires **once per
visit**: it has to leave and stay away ten minutes before it can interrupt
again.

| Trigger | Colour | Detected from |
|---------|--------|---------------|
| Emergency | red | squawk 7500 (hijack), 7600 (radio failure), 7700 (general), or a declared `emergency` |
| Military | amber | the feed's `dbFlags` military bit |
| Notable type | cyan | the list below |

**Notable types** (`kNotableTypes` in `src/util/aircraft_class.cpp`):

`A124` An-124 · `A225` An-225 · `A337` BelugaXL · `A388` **A380** ·
`A3ST` Beluga · `A400` A400M · `AN22` An-22 · `B52` B-52 · `B703` Boeing 707 ·
`B741`–`B748`, `B74D`, `B74R` **Boeing 747 family** · `B74S` Dreamlifter ·
`C5M` C-5 Galaxy · `CONC` Concorde · `IL76`, `IL86`, `IL96` Ilyushin

The list is deliberately short — a false alarm is more annoying than a miss.
Adding a type is one line in that file.

### Where the data comes from

Aircraft come over HTTPS from [adsb.fi](https://adsb.fi) (its open data API,
v3) first, with [adsb.lol](https://api.adsb.lol/docs) as the reserve. After two
failures in a row the radar switches to the other feed, and goes back to
adsb.fi as soon as it answers. A feed that refuses (HTTP 400, 401, 403 or 404)
is rested for hours; one that says "too many requests" (429) is left alone for
as long as it asks. adsb.lol allows 500 requests a day per address, so the
radar rations it: the first 60 of a day go at the usual 10 s, and after that
one every 4.5 minutes at most, which keeps a whole day without adsb.fi under
400 (the hard limit). Off adsb.fi the radar tries it again after five
minutes, or after 30 s while adsb.lol's ration holds it back, so a short
adsb.fi outage passes without "No flight data". In a long one aircraft come
every 4.5 minutes and are hidden after 90 s, as where they are drawn is then
a guess; the screen says "No flight data" and when the next update is due.
This feed state survives a restart, so a refusal or a ration is not forgotten
and asked about again at once.
The radar polls about every 10 s, and the radius grows with the selected range so
rim dots have something to show. Aircraft on the ground are hidden by default
(`kAdsbShowGroundAircraft` in `include/config.h`).

Routes come from [adsb.im](https://adsb.im). Town and ZIP code lookups use
Open-Meteo's geocoding.

---

## Heat and stability

The board runs warm — around 70 °C on the chip is normal for a radio that never
sleeps inside a closed printed case. It is well inside the part's rating, and
one setting lets you trade.

Halving the core clock to 80 MHz was tried and **measured no difference** —
still about 70 °C. The heat comes from the radio, which is kept permanently
awake so the connection does not drop, not from the core. The setting has been
removed rather than kept as a control that costs responsiveness and buys
nothing; the core runs at its full 160 MHz.

**Wi-Fi transmit power** is capped at 8.5 dBm by default — inherited from the
original, and a sensible cap for the Super Mini's small regulator. If the
connection drops in a weak spot, raise it to 13 or 19 dBm. That costs current
and therefore heat, so change one thing at a time.

The chip temperature is on the **System** page of the settings site, and is also
written to the serial log every minute (115200 baud), so you can measure the
effect rather than guess it.

**Short Wi-Fi drops** no longer take the screen away: the radar stays up for the
first 25 seconds of an outage while the ESP32's own auto-reconnect does its
work. Only a longer outage brings up the connecting animation.

---

## Security

The device sits on a home network and its settings page has no login, so the
aim is to keep the reachable surface as small as the feature set allows.

| Measure | Why |
|---------|-----|
| `/update`, `/u`, `/erase`, `/restart` return 404 | WiFiManager registers an **unauthenticated firmware-upload page** plus credential-wipe and reboot endpoints on every portal, and links the upload form from its menu. Its own authentication hook is a no-op in 2.0.17. Nothing here needs those routes: an update is fetched and signature-checked by the radar itself, never uploaded to it, and a credential reset is a 10 s BOOT hold. The web-server callback runs before the library registers its routes and ESP32's `WebServer` uses the first match, so claiming the URIs there shadows them for good. |
| Setup AP is WPA2-protected | An open AP lets anyone in radio range hand the radar a network of their choice. The password is derived from the board's MAC, so it is stable and printable on the setup screen. |
| Only signed, newer updates | The radar installs an update only if its ECDSA (P-256) signature checks out against the public key built into the firmware, its SHA-256 matches, and its version is strictly newer than the running one. The signing key is never in the repository: it is an encrypted repository secret that only the release workflow reads, plus the maintainer's backup. A new image runs on trial and rolls back by itself if it fails ([docs/OTA.md](docs/OTA.md)). |
| Flight data and updates over HTTPS, certificates checked | Each server's certificate is checked against an embedded root bundle (Mozilla's list, `data/cert/`). If one server's certificate is rejected six times in a row, the radar stops checking that server rather than going dark for good; the settings page then says "certificate not checked". A monthly CI job checks every server against the bundle so a change on their side is noticed first. For updates the signature protects the image either way. |
| Response size and memory caps | The chip has 320 KB of RAM and a TLS connection takes about 45 KB, so the frame is drawn in two 120-row halves rather than from one 115 KB buffer. The connection's two 16.7 KB record buffers are set aside once, as the radar starts, and lent to every connection (`services::tls`), so a heap cut into pieces by a day's running no longer makes HTTPS fail for want of them. The reply is stream-parsed rather than buffered: the aircraft list one aircraft at a time (`util::feed_answer`), each through a field filter and within a memory limit of its own (`util::JsonBudget`, 4 KB), so an answer of any size takes the memory of one aircraft, and the nearest 64 are kept. Reading also stops while about 10 KB of the heap Wi-Fi can use is still free, unless the block fits in the RTC heap Wi-Fi cannot use (asked before each block), and after 128 KB on the wire or 8 s; an answer cut short shows the aircraft read so far, rather than the heap running out. A list mostly from outside the area asked about counts as that feed failing. Route, town search and update answers have a memory limit of their own, and route and town search answers are given up after 8 s or 32 KB. |
| `/screen.bmp` is read-only | It shows anyone on your network what the radar shows — the same picture anyone in the room can see. |
| Strict input parsing | Coordinates, checkboxes and every number field are validated in `include/util/`; a malformed field leaves the stored setting untouched. Covered by host tests. |
| Settings form carries a token | A browser attaches no origin restriction to a plain form submit, so any web page you happened to have open could otherwise POST new settings to the radar. The settings page carries a random per-boot token that a foreign page cannot read, and a save without it is refused. |

**Still plain HTTP:** route lookups (adsb.im) and the town and ZIP code search
(Open-Meteo) start on plain HTTP, and switch to HTTPS by themselves if the
server redirects. A route lookup sends callsigns and aircraft positions, and the
search sends the town you typed; neither sends your coordinates. Someone able to
see or manipulate your network traffic could feed the radar a false route or a
wrong town. They cannot reach further — the parser is bounded, every string
copy is length-checked, and the reply is capped. Aircraft positions, which are
asked for with your coordinates, always go over HTTPS.

**Also open by design:** anyone already on your network can change the settings.
WiFiManager offers no working authentication, so the only real alternatives are
a time-limited settings page or none at all.

---

## Quality pipeline

Five stages, cheapest first — see [docs/QUALITY.md](docs/QUALITY.md) for the
detail and the hardware checklist.

1. **Compiler warnings** — `-Wall -Wextra`; CI fails on any warning in this
   project's own code.
2. **Host tests** — `pio test -e native`, over the input parsers, the callsign
   parser, the aircraft classifier and the alert rules, the guided setup, the
   feed failover, the update rules and the logo seam, plus a render of the
   settings page that fails if any control or the Save button goes missing. The
   release tools have Python tests of their own in `tools/ota/`. They run on the
   build machine, so hostile and malformed inputs are exercised without a board.
3. **Static analysis** — cppcheck over `src/` and `include/`, failing on any
   finding.
4. **Firmware build** — plus the web-flashable image, published as an artifact.
5. **Hardware smoke test** — a written checklist for what no machine can see.

It has already earned its keep: stage 1 found dead code carrying the wrong
safety margin, stage 3 found a portability bug in the trail store, stage 2 now
guards the truncated settings page that no compiler could see, and two defects
in the test harness itself surfaced — including assertions that silently
compared nothing.

---

## Project internals

### Regenerating the built-in data

| Command | Produces | Source |
|---------|----------|--------|
| `python3 scripts/build_ui_fonts.py` | the four `.vlw` fonts | Noto Sans SemiBold (OFL) |
| `python3 scripts/build_airlines.py` | the airline table | OpenFlights, **ODbL** |
| `python3 scripts/build_large_airports.py` | the runway dataset | OurAirports |
| `python3 scripts/build_aircraft_types.py` | type code → name ("B38M" → "Boeing 737 MAX 8") | curated in the script |
| `python3 scripts/build_airline_brands.py` | airline short names, IATA codes and badge colours for `data/airline_brand_codes.txt` (`--logos`: also the private logo pack, `.cache/logo-pack.bin`) | OpenFlights (ODbL), colours curated in the script; logos are **trademarks**, see above. `--logos` needs Pillow |
| `python3 scripts/build_water.py --lat .. --lon ..` | the water layer around a point | OpenStreetMap, **ODbL**. Needs shapely. Re-run after moving the radar elsewhere |
| `python3 scripts/build_cert_bundle.py` | the root certificate bundle, `data/cert/x509_crt_bundle.bin` | Mozilla's CA list (MPL-2.0), as published by curl. Needs `cryptography` |

Fonts need `pip install freetype-py`. CI rebuilds the fonts and fails if they
differ from what is committed. The airline table is not diffed, because it
tracks a live upstream dataset.

### Where things live

```
include/
  config.h                 — pins, timing, defaults
  ota_public_key.h         — the public key updates are checked against (generated)
  util/                    — Arduino-free logic, covered by host tests
  hardware/                — display and font plumbing
  data/                    — generated tables (airports, airlines)
  ui/                      — radar drawing, settings, alerts, trails
  services/                — Wi-Fi portal, ADS-B client, location, name and welcome, updates
src/                       — the matching implementations
data/                      — embedded font files, and the certificate bundle (cert/)
partitions/                — flash layout: two app slots, and the private logos partition
scripts/                   — dataset, font and certificate generators, build checks, release tools
site/                      — web installer and update manifest (GitHub Pages)
test/test_util/            — host unit tests
test/test_setup/           — host unit tests for the guided setup
test/test_feeds/           — host tests for feed failover, adsb.lol's allowance and pace, and days of outages run through them
test/test_json_budget/     — host tests for the memory budget of servers' answers
test/test_feed_answer/     — host tests for reading a feed's answer one aircraft at a time, and judging it
test/test_ota/             — host tests for the update rules
test/test_logos/           — host tests: airline badge, brand table, logo seam
tools/ota/                 — release tool tests (sign and verify, refusals)
tools/host_checks/         — renders the settings page on the build machine
docs/QUALITY.md            — the five-stage pipeline and hardware checklist
docs/OTA.md                — the over-the-air update contract
docs/RELEASING.md          — how a release is signed and published
```

### CI

| Workflow | When | Output |
|----------|------|--------|
| [Build](.github/workflows/build.yml) | push, PR, manual | `plane-radar-supermini` artifact with the flashable images |
| [Quality](.github/workflows/quality.yml) | push, PR, manual | unit tests, static analysis, font regeneration check, no-logos check |
| [Release](.github/workflows/release.yml) | git tag `vX.Y.Z` | tests and builds the tag, signs it with the `OTA_SIGNING_KEY` secret, commits `site/` to `main` and deploys Pages |
| [Pages](.github/workflows/pages.yml) | push to `main` touching `site/` | verifies the signed release like a radar would, then deploys the installer and update manifest |
| [Certificate check](.github/workflows/cert-check.yml) | monthly, manual | checks every server the radar talks to against the embedded root bundle |
| [API watch](.github/workflows/api-watch.yml) | daily, manual | asks every service the radar depends on (aircraft feeds, routes, town search, update site, certificates) the way the radar does, and opens an issue when one changes or goes away ([`tools/api_watch.py`](tools/api_watch.py)) |

---

## Credits and licence

- Original project: **[MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar)** — MIT licence, retained in [LICENSE](LICENSE). The hardware design, the case and the radar this work builds on are theirs.
- The extended firmware this version builds on: **[fabsface-max/ESP32-Plane-Radar](https://github.com/fabsface-max/ESP32-Plane-Radar)** — the display options, portal hardening and quality pipeline listed above.
- Aircraft data: [adsb.fi](https://adsb.fi) open data API, and [adsb.lol](https://adsb.lol) (ODbL) · Routes: [adsb.im](https://adsb.im) · Water: © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors (ODbL) · Airline logos (private logo pack only, not distributed): the airlines' trademarks, via FlightAware · Airports: [OurAirports](https://ourairports.com/) · Airlines: [OpenFlights](https://openflights.org/data.html) (ODbL) · Town and ZIP code lookup: [Open-Meteo](https://open-meteo.com/) geocoding API, location data based on [GeoNames](https://www.geonames.org/) (CC BY 4.0) · Font: Noto Sans (OFL) · Root certificates: [Mozilla's CA list](https://curl.se/docs/caextract.html) (MPL-2.0)
- Libraries: [LovyanGFX](https://github.com/lovyan03/LovyanGFX), [WiFiManager](https://github.com/tzapu/WiFiManager), [ArduinoJson](https://github.com/bblanchon/ArduinoJson); the browser installer uses [ESP Web Tools](https://github.com/esphome/esp-web-tools)
- The additions after the original were designed and implemented with [Claude Code](https://claude.com/claude-code), then flashed and checked on hardware before being kept.
