# Changelog

Releases are installed over the air by radars running an earlier release
(docs/OTA.md). Newest first.

## v2.0.3

- First update delivered over the air from GitHub Pages (no functional
  change).

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
