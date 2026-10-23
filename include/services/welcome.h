#pragma once

/**
 * The radar's name and its one-time welcome.
 *
 * A radar given as a present may be set up by someone who has never seen one:
 * the giver resets it for a new owner, keeping the location, and the new owner
 * only joins it to their Wi-Fi. The first time it then has Wi-Fi and a
 * location it says "Welcome to <name>" and offers a short guide (/guide on
 * the settings server) instead of going straight to the radar.
 *
 * Everything here lives in its own NVS namespace, which the reset for a new
 * owner erases with the rest: the welcome is then due again. Main loop only.
 */
namespace services::welcome {

/** Load the name and the flags. Call once in setup(), before Wi-Fi. */
void init();

/** The name the owner gave the radar ("Ada's radar"); "" when it has none. */
const char* name();

/**
 * From the settings form: checked by util::portal::radarName, then stored and
 * applied. Empty clears the name. False, changing nothing, when refused.
 */
bool saveName(const char* value);

/** True until the welcome has been shown on this radar (since the last reset). */
bool due();

/** The welcome has been shown or skipped: not again until a reset for a new owner. */
void markShown();

/** /guide was opened: the welcome has done its job, and settings stops pointing at it. */
void noteGuideOpened();

/** Latched: /guide was opened since the last call. */
bool consumeGuideOpened();

/** True once /guide has been opened on this radar (since the last reset). */
bool guideSeen();

/** Forget the name and that the welcome was shown (reset for a new owner). */
void clear();

}  // namespace services::welcome
