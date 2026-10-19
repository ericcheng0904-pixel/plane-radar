#pragma once

#include "util/setup_text.h"

/*
 * Full-screen messages, drawn straight to the panel (not through the radar's
 * frame sprite). The setup screens walk a person with only a phone through
 * two steps: join the radar's hotspot and pick the home Wi-Fi, then open the
 * settings page and say where the radar is.
 */

/**
 * Setup step 1: a QR code that joins the phone to the setup hotspot, with a
 * hint that a tap shows the password instead. Says "Step 1 of 2" while no
 * location is saved, plain "Wi-Fi setup" when only the Wi-Fi is missing.
 */
void statusScreenPortal();
/** Step 1, the other view: the hotspot's name and password in words. */
void statusScreenPortalText();
/** Step 1 once a phone has joined the hotspot: carry on on the phone. */
void statusScreenPortalPhone();
/**
 * The hotspot is open because the saved network is out of reach; it is
 * retried every few minutes. `password_suspect`: the last attempt ended the
 * way a changed password does, so say that too.
 */
void statusScreenPortalRetrying(const char* ssid, bool password_suspect);
/** Joining `ssid` failed; `why` picks the advice. */
void statusScreenJoinFailed(const char* ssid, util::setup::JoinFailure why);

/** Setup step 2: a QR code for the settings page at `ip`, and the address. */
void statusScreenLocationStep(const char* ip);
/** Shown for a few seconds after a location is saved: where the radar looks now. */
void statusScreenLocationSaved(const char* place);

/** Wi-Fi gone for longer than a hiccup. Tick keeps the spinner turning. */
void statusScreenWifiLost(const char* ssid);
void statusScreenWifiLostTick();

void statusScreenConnectFailed();
void statusScreenWifiReset();

/** An update is downloading; the radar restarts by itself when done. */
void statusScreenUpdating();

/**
 * The info screen (hold the button 2-9 s): a QR code and the address of the
 * settings page, the firmware version and how the data is doing. For help
 * over the phone: "what does it say under the code?"
 */
void statusScreenInfo(const char* ip, const char* version_line, const char* status_line);
/** Reset for a new owner: everything erased, restarting into setup. */
void statusScreenErased();

/**
 * While BOOT is held over a status screen: the countdown to the Wi-Fi reset,
 * as the radar shows it. When the button is let go the screen underneath is
 * drawn again. Call on every pass with bootButtonHeldMs().
 */
void statusScreenHoldOverlay(unsigned long held_ms);

/** Saved-network connect animation (call Tick until connect finishes). */
void statusScreenConnectingBegin(const char* ssid);
void statusScreenConnectingTick();
