#pragma once

/**
 * Self-repair glue around util::health: feeds it the radar's real state and
 * restarts the device when a rule fires, remembering why across the restart.
 *
 * The main loop and the fetch task watch each other: the loop restarts the
 * radar if fetching stopped, the fetch task restarts it if the loop stopped.
 */
namespace services::health {

/** Call once early in setup(): reads why the last restart happened. */
void begin();

/** Main loop is alive. Also called from long waits that block the loop. */
void heartbeat();

/** From the main loop, every pass: checks fetching, data, memory, uptime. */
void checkFromLoop();

/** From the fetch task, every cycle: checks that the main loop still runs. */
void checkFromFetchTask();

/** "power on", "crash", "flight data task stopped", ... for the status line. */
const char* lastRestartText();

}  // namespace services::health
