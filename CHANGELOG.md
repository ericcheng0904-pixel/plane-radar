# Changelog

Releases are installed over the air by radars running an earlier release
(docs/OTA.md). Newest first.

## v2.0.7

- Release used to test the trial boot of v2.0.6's code (no change).

## v2.0.6

- After Wi-Fi is set up from a phone, the radar shows "Connected" and
  restarts once: straight on from the setup hotspot, every flight fetch
  failed until the next restart, so a new owner saw an empty sky.
- When a location is already set, the settings page shows where the radar
  is and folds the location form away under "Change the location".

## v2.0.5

- Release used to test updates from GitHub Pages (no functional change).

## v2.0.4

- The update's memory check runs after the frame buffer is handed back: a
  settings page being served at the moment of the check made v2.0.3 wait.

## v2.0.3

- Release used to test updates from GitHub Pages (no functional change).

## v2.0.2

- Updates free the screen's frame buffer while they download (it showed
  "Updating" and restarts either way): with the frame buffer, a TLS session
  and the update buffer together, memory ran out and the download failed.
- Airline logos are read straight from flash; the screenshot buffer is only
  taken while a screenshot is being sent: ~6 KB more free memory.

## v2.0.1

- Continuous-integration fixes.

## v2.0.0

- First release of Plane Radar 2: HTTPS feeds with failover, guided phone
  setup, signed over-the-air updates, aircraft shapes by kind, night dimming.
