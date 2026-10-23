# Changelog

Releases are installed over the air by radars running an earlier release
(docs/OTA.md). Newest first.

## v2.0.14

- Fix: the radar restarted itself now and then (reason "low memory"), about
  once in a day's running. Every HTTPS request needs two 16.7 KB record
  buffers, and they were looked for anew each time. After hours of running,
  the free memory was in smaller pieces, and a small block left in the one
  piece that was still big enough made the next requests fail ("SSL - Memory
  allocation failed"): three in a row restart the radar. The two buffers are
  now set aside once, as the radar starts, and lent to every request. In
  hardware fault tests, a heap cut into pieces that failed every request
  before now keeps the flights coming.
- A radar that really is short of memory still skips its requests and
  restarts itself, as before. The bar is now the ~28 KB a request needs
  besides the buffers, including Wi-Fi's share: a request begun with less in
  a fault test starved Wi-Fi and timed out.
- Fix: a certificate check that ran out of memory part way counted as the
  server's certificate failing to verify, and six in a row would have
  stopped the radar checking that server. It now counts as the radar being
  short of memory.

## v2.0.13

- A radar can have a name (Settings, Display: "Name of this radar", optional,
  up to 32 plain characters; a phone's curly quotes are fine).
- New owners are welcomed. The setup page on the phone says in one line what
  the radar is. The first time the radar is on Wi-Fi with a location, the
  screen says "Welcome to" its name (or "your Plane Radar") and shows a QR
  code for a short guide; a tap on the button skips it, and it goes by
  itself after 3 minutes. Radars updating from an earlier version show it once
  too.
- The guide is a new page, `/guide` (**Guide** in the menu): what the radar
  is, how to read the screen, the button, and what to do when something looks
  wrong. Settings points at it until it has been opened once.
- Reset for a new owner: "Keep the location" now keeps the radar's name too.
  Do it last: the welcome shows once, to whoever sets the radar up next.
- Fix: opening the settings page could restart the radar ("A stack overflow
  in task loopTask", then "last restart: crash"). The page was built in large
  text buffers on the main loop's 8 KB stack, and with this release's
  additions a request needed about 8.5 KB. Pages are now written straight
  into one small buffer borrowed for the request, and the settings page now
  needs about 3 KB. A build check (`scripts/check_stack_usage.py`) now fails on
  any function with an oversized stack frame, and the serial log reports the
  main loop's spare stack ("loop stack left").
- Pages load faster. The settings page took about 5 seconds: it went out in
  some sixty small pieces, and the radar waited on the network for every few
  of them. It now goes out in six full-size ones. The pages' text is
  unchanged.

## v2.0.12

- Release used to test v2.0.11's updater end to end (no change).

## v2.0.11

- Fix: in a long adsb.fi outage the reserve, adsb.lol, was used up for the
  day in about 75 minutes, and the radar then had no aircraft for most of a
  day. adsb.lol is now rationed: the first 60 requests of a day at the usual
  10 s, then one every 4.5 minutes, so a whole day stays under 400 (adsb.lol
  allows 500). While adsb.lol waits, adsb.fi is tried every 30 s, so a short
  adsb.fi outage passes without "No flight data". In a long one the screen
  says when the next update is due ("next update in 3 min") instead of
  "retrying...".
- Fix: saving the display settings or a new location after the radar had
  been on Wi-Fi for 30 minutes restarted it at once ("no flight data for 30
  min"): emptying the screen for the new settings looked like a feed that
  had sent nothing.
- Fix: after an update check or a town search the next flight fetch could
  wait up to a minute more, enough to trip the "flight data task stopped"
  restart. It now follows within 10 s.
- Fix: when the heap was cut into pieces too small for mbedTLS's two
  16.7 KB record buffers, every HTTPS request failed ("SSL - Memory
  allocation failed") and the radar blamed the feeds: it switched between
  them and backed off until the 30-minute no-data restart. Found by fault
  tests against a local mock feed. Such a failure no longer counts against
  a feed, and three in a row restart the radar (reason "low memory"), which
  puts the heap back together in a few seconds.
- Redirects from a feed are no longer followed: HTTPClient opened the second
  connection while the first one's strings still sat where the record
  buffers go, so it always failed the same way. A feed that moves now counts
  as failing (the other one takes over) and the daily API watch reports the
  new address.
- Fix: an answer far bigger than the one asked for (a fault test sent 3000
  aircraft) used up the memory Wi-Fi and the display need, and could crash
  the radar. The aircraft list is now read one aircraft at a time, so an
  answer of any size takes the memory of one aircraft (under 2 KB), where a
  weekday evening's at the widest range took 20-24 KB. Reading also stops
  while about 10 KB of the memory Wi-Fi can use is still free, unless the
  aircraft fits in the RTC memory Wi-Fi cannot use (filled first).
- With more than 64 aircraft in range, the 64 nearest are shown, not the
  first 64 of the feed's list.
- An answer over 128 KB, still arriving after 8 s, or holding an aircraft
  record over 4 KB is cut short, and the aircraft read up to there are shown
  ("answer cut short" in the log). The time then comes from the answer's
  Date header, as the feed's clock comes after the list, so the screen still
  dims at night. Only the 128 KB cut is the feed answering a busy sky; a
  stall, an early close or an oversized record counts against the feed, so
  one that keeps doing it hands over to the other.
- Route, town search and update answers have a memory limit of their own;
  those too big are dropped. Route and town search answers are also given up
  after 8 s or 32 KB.
- An aircraft list mostly from far outside the area asked about (a feed that
  ignores the radius) counts as that feed failing, so the other one takes
  over; aircraft that far out are not drawn.
- An answer the radar had no memory left to read no longer counts against the
  feed; like the failed handshakes above, three in a row restart the radar.
- A flight fetch skipped for lack of room for an HTTPS connection now counts
  the same way; a radar with its memory cut up stayed dark until the
  30-minute no-data restart.
- A feed clock ("now") out of range, such as one sent in microseconds, no
  longer sets the radar's clock to the year 2106; the Date header stands in.

## v2.0.10

- First release built, signed and published by GitHub Actions from a tag
  (no functional change).

## v2.0.9

- Fix: a new release on trial was rolled back the moment Wi-Fi came up
  whenever the clock read an even number of milliseconds (half of all
  updates; v2.0.6 and v2.0.8 were rolled back this way).

## v2.0.8

- "Check for an update now" checks 15 s after the button, once the page has
  been sent: a check started while the page was still being served could
  run short of memory and fail (HTTP -1).

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
