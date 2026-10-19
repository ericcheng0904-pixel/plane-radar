#pragma once

#include <cstddef>

/**
 * Signed over-the-air updates from the project's GitHub Pages, as specified
 * in docs/OTA.md. The point is not new features but keeping a radar that
 * nobody maintains working when a data feed changes under it: about once a
 * day the radar looks for a newer release, and installs it only if it is
 * signed with the maintainer's key. A new image then runs on trial and rolls
 * back by itself unless Wi-Fi, the display and a flight fetch all work.
 *
 * Network work happens on the fetch task, in place of an aircraft fetch, so
 * there is never a second TLS connection.
 */
namespace services::ota {

/** At boot, before the radar starts: note a trial run, settle the last attempt. */
void begin();

/**
 * Fetch task, once per cycle: check for (and install) an update when one is
 * due. True when it used the network this cycle. Does not return when it
 * installs one: the radar restarts into it.
 */
bool runIfDue();

/** Main loop: the radar has drawn a frame (one of the trial's conditions). */
void noteRadarDrawn();

/** Main loop, every pass: confirm a trial run, or roll it back after 10 min. */
void checkTrial();

/** Settings page "Check now": look on the next fetch-task cycle. */
void requestCheck();

/** One line for the settings page ("Up to date (checked 2 h ago)"). */
void statusText(char* out, size_t n);

}  // namespace services::ota
