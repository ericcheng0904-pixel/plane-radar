#include "ui/status_screens.h"

#include <lgfx/v1/lgfx_fonts.hpp>
#include <lgfx/utility/lgfx_qrcode.h>
// lgfx_qrcode.h defines generic macro names; LovyanGFX drops them after
// including it, and so does this file.
#undef MODE_NUMERIC
#undef MODE_ALPHANUMERIC
#undef MODE_BYTE
#undef ECC_LOW
#undef ECC_MEDIUM
#undef ECC_QUARTILE
#undef ECC_HIGH
#undef LOCK_VERSION

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/panel_color.h"

namespace {

constexpr int kLineGap = 6;
const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;

constexpr int kSpinnerDotCount = 10;
constexpr int kSpinnerRadius = 113;
constexpr int kSpinnerDotRadius = 2;
constexpr int kSpinnerEraseRadius = 4;
constexpr float kSpinnerStepDeg = 6.0f;

struct SpinnerDot {
  int x = 0;
  int y = 0;
  bool drawn = false;
};

char s_connecting_ssid[33];
char s_ssid_line[33];
constexpr int kConnectingTextMaxWidthPx = 220;
float s_spinner_angle_deg = -90.0f;
SpinnerDot s_spinner_dots[kSpinnerDotCount];
bool s_connecting_text_drawn = false;
/** The spinner runs on the black connecting screen and on the setup palette. */
uint16_t s_spinner_bg = config::kColorBlack;
bool s_spinner_setup_colours = false;

// The setup screens' one calm palette: the radar's navy, white for what to
// do, grey-blue for the small print, amber for "where you are in the steps"
// (as the radar's own notices), and a warm red only when something failed.
const ui::Rgb kSetupBg{10, 22, 40};
const ui::Rgb kSetupText{255, 255, 255};
const ui::Rgb kSetupMuted{165, 182, 205};
const ui::Rgb kSetupStep{255, 190, 90};
const ui::Rgb kSetupBad{255, 128, 104};
const ui::Rgb kSetupGood{125, 222, 150};
/** Spinner dots on the setup palette fade from this towards the background. */
const ui::Rgb kSetupSpinner{150, 175, 210};

constexpr auto& kGfxTitle = fonts::FreeSans18pt7b;
constexpr auto& kGfxBody = fonts::FreeSans12pt7b;
constexpr auto& kConnectingGfxDetail = fonts::FreeSans9pt7b;

struct TextLine {
  const char* text;
  float vlw_size;
  const lgfx::GFXfont* gfx_font;
};

/**
 * Status screens scale the largest embedded font: they are short-lived and
 * their text is far bigger than the radar labels, so the coarser scaling does
 * not show. Pin the size explicitly — the radar leaves whichever font its own
 * labels last needed loaded on the panel.
 */
void setSmoothSize(float size) {
  displayFontEnsureLoaded(tft, kUiFont15);
  displayFontSetSmoothSize(tft, size);
}

int lineHeightGfx(const lgfx::GFXfont* font) {
  displayFontSetBitmap(tft, font);
  return tft.fontHeight();
}

int lineHeightVlw(float size) {
  setSmoothSize(size);
  return tft.fontHeight();
}

void applyLineStyle(const TextLine& line) {
  if (displayFontIsSmooth()) {
    setSmoothSize(line.vlw_size);
  } else {
    displayFontSetBitmap(tft, line.gfx_font);
  }
}

void drawTextBlock(uint16_t bg, uint16_t fg, const TextLine* lines, size_t count) {
  tft.fillScreen(bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(textdatum_t::middle_center);

  int total_h = 0;
  for (size_t i = 0; i < count; ++i) {
    if (displayFontIsSmooth()) {
      total_h += lineHeightVlw(lines[i].vlw_size);
    } else {
      total_h += lineHeightGfx(lines[i].gfx_font);
    }
    if (i + 1 < count) {
      total_h += kLineGap;
    }
  }

  int y = (config::kDisplayHeight - total_h) / 2;
  for (size_t i = 0; i < count; ++i) {
    applyLineStyle(lines[i]);
    const int h =
        displayFontIsSmooth() ? lineHeightVlw(lines[i].vlw_size)
                              : lineHeightGfx(lines[i].gfx_font);
    tft.drawString(lines[i].text, kCenterX, y + h / 2);
    y += h + kLineGap;
  }
}

constexpr float kConnectingDetailVlw = 0.92f;

void applyConnectingDetailStyle() {
  if (displayFontIsSmooth()) {
    setSmoothSize(kConnectingDetailVlw);
  } else {
    displayFontSetBitmap(tft, &kConnectingGfxDetail);
  }
}

/** SSID on one line; truncate with "..." if wider than kConnectingTextMaxWidthPx. */
void fitSsidLine() {
  strncpy(s_ssid_line, s_connecting_ssid, sizeof(s_ssid_line) - 1);
  s_ssid_line[sizeof(s_ssid_line) - 1] = '\0';
  applyConnectingDetailStyle();
  if (tft.textWidth(s_ssid_line) <= kConnectingTextMaxWidthPx) {
    return;
  }
  // ASCII dots: the embedded fonts have no ellipsis glyph.
  const size_t len = strlen(s_connecting_ssid);
  for (size_t n = len; n > 0; --n) {
    snprintf(s_ssid_line, sizeof(s_ssid_line), "%.*s...", static_cast<int>(n),
             s_connecting_ssid);
    if (tft.textWidth(s_ssid_line) <= kConnectingTextMaxWidthPx) {
      return;
    }
  }
  strncpy(s_ssid_line, "...", sizeof(s_ssid_line) - 1);
  s_ssid_line[sizeof(s_ssid_line) - 1] = '\0';
}

void drawConnectingText() {
  tft.fillScreen(config::kColorBlack);

  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(config::kTextOnBlack, config::kColorBlack);

  applyConnectingDetailStyle();
  const int detail_h = tft.fontHeight();
  const int total_h = detail_h * 2 + kLineGap;
  const int block_top = (config::kDisplayHeight - total_h) / 2;
  constexpr int kPanelPadY = 8;
  tft.fillRect(kCenterX - kConnectingTextMaxWidthPx / 2, block_top - kPanelPadY,
               kConnectingTextMaxWidthPx, total_h + kPanelPadY * 2, config::kColorBlack);

  int y = block_top;
  tft.drawString("Connecting to", kCenterX, y + detail_h / 2);
  y += detail_h + kLineGap;
  tft.drawString(s_ssid_line, kCenterX, y + detail_h / 2);

  s_connecting_text_drawn = true;
}

void eraseSpinnerDots() {
  for (int i = 0; i < kSpinnerDotCount; ++i) {
    if (!s_spinner_dots[i].drawn) {
      continue;
    }
    tft.fillCircle(s_spinner_dots[i].x, s_spinner_dots[i].y, kSpinnerEraseRadius,
                   s_spinner_bg);
    s_spinner_dots[i].drawn = false;
  }
}

void drawSpinnerDots() {
  constexpr float kDegToRad = 0.01745329252f;
  const float head_rad = s_spinner_angle_deg * kDegToRad;

  for (int i = 0; i < kSpinnerDotCount; ++i) {
    const float a = head_rad - static_cast<float>(i) * (6.283185307f / kSpinnerDotCount);
    const int x = kCenterX + static_cast<int>(std::lround(std::cos(a) * kSpinnerRadius));
    const int y = kCenterY + static_cast<int>(std::lround(std::sin(a) * kSpinnerRadius));

    const int fade = 255 - i * 22;
    const uint16_t color =
        s_spinner_setup_colours
            ? ui::glassColor(ui::mix(kSetupBg, kSetupSpinner, static_cast<float>(fade) / 255.0f))
            : tft.color565(0, fade, 0);
    tft.fillSmoothCircle(x, y, kSpinnerDotRadius, color);

    s_spinner_dots[i].x = x;
    s_spinner_dots[i].y = y;
    s_spinner_dots[i].drawn = true;
  }
}

void resetSpinner(uint16_t bg, bool setup_colours) {
  s_spinner_angle_deg = -90.0f;
  for (auto& dot : s_spinner_dots) {
    dot.drawn = false;
  }
  s_spinner_bg = bg;
  s_spinner_setup_colours = setup_colours;
}

void tickSpinner() {
  eraseSpinnerDots();
  s_spinner_angle_deg += kSpinnerStepDeg;
  if (s_spinner_angle_deg >= 270.0f) {
    s_spinner_angle_deg -= 360.0f;
  }
  drawSpinnerDots();
}

// --- Setup screens ---------------------------------------------------------

/** One text role: an embedded font at a scale, and the bitmap font standing in for it. */
struct Style {
  size_t font;
  float scale;
  const lgfx::GFXfont* fallback;
};

constexpr Style kTitle{kUiFont15, 1.3f, &fonts::FreeSansBold12pt7b};
/** Twice the 15 px font: a whole-number scale keeps the password's strokes even. */
constexpr Style kBig{kUiFont15, 2.0f, &fonts::FreeSansBold18pt7b};
constexpr Style kBody{kUiFont15, 1.0f, &fonts::FreeSans9pt7b};
constexpr Style kSmall{kUiFont13, 1.0f, &fonts::FreeSans9pt7b};
constexpr Style kTiny{kUiFont11, 1.0f, &fonts::FreeSans9pt7b};

/** Each module of a QR code is this many pixels: 4 keeps a version 3 code inside the glass. */
constexpr int kQrModulePx = 4;
/**
 * Light border, in modules. The standard asks for 4; phone cameras read 2 on
 * a lit screen without trouble, and the two saved leave room for the hint
 * below the code at a readable size on the round glass.
 */
constexpr int kQrQuietModules = 2;
constexpr int kQrCenterY = 118;

/** Redraws whichever setup screen is up, after the hold overlay covered it. */
void (*s_redraw)() = nullptr;
/** Seconds the hold overlay last showed, plus one; 0 = no overlay on screen. */
unsigned long s_overlay_secs = 0;
char s_arg_ssid[33] = "";
char s_arg_text[48] = "";
util::setup::JoinFailure s_arg_why = util::setup::JoinFailure::kNone;
bool s_arg_flag = false;

void applyStyle(const Style& s) {
  if (displayFontIsSmooth()) {
    displayFontEnsureLoaded(tft, s.font);
    if (s.scale != 1.0f) {
      displayFontSetSmoothSize(tft, s.scale);
    }
  } else {
    displayFontSetBitmap(tft, s.fallback);
  }
}

/** Width of the round glass at row `y`, less a margin each side. */
int glassWidthAt(int y) {
  const float dy = static_cast<float>(y - kCenterY);
  const float r = static_cast<float>(config::kDisplayWidth) / 2.0f;
  const float half = dy * dy >= r * r ? 0.0f : std::sqrt(r * r - dy * dy);
  return static_cast<int>(2.0f * half) - 16;
}

/**
 * One centred line at row `y`. Text too wide for the glass there steps down
 * a font size at a time; what still does not fit (a long network name) is
 * cut with "..." rather than run off the round edge.
 */
void line(const char* text, int y, Style style, const ui::Rgb& colour) {
  tft.setTextDatum(textdatum_t::middle_center);
  int room = 0;
  for (;;) {
    applyStyle(style);
    const int h = tft.fontHeight();
    room = std::min(glassWidthAt(y - h / 2), glassWidthAt(y + h / 2));
    if (tft.textWidth(text) <= room) {
      tft.setTextColor(ui::glassColor(colour), ui::glassColor(kSetupBg));
      tft.drawString(text, kCenterX, y);
      return;
    }
    if (!displayFontIsSmooth() || (style.scale == 1.0f && style.font + 1 >= kUiFontCount)) {
      break;
    }
    style = style.scale != 1.0f ? Style{style.font, 1.0f, style.fallback}
                                : Style{style.font + 1, 1.0f, style.fallback};
  }
  tft.setTextColor(ui::glassColor(colour), ui::glassColor(kSetupBg));
  char cut[48];
  for (size_t n = strnlen(text, sizeof(cut) - 4); n > 0; --n) {
    snprintf(cut, sizeof(cut), "%.*s...", static_cast<int>(n), text);
    if (tft.textWidth(cut) <= room) {
      break;
    }
  }
  tft.drawString(cut, kCenterX, y);
}

void clearSetup() { tft.fillScreen(ui::glassColor(kSetupBg)); }

/**
 * `text` as a QR code centred at (kCenterX, cy) on a white rounded plate,
 * which doubles as the light quiet zone the code needs around it. The
 * library's encoder is asked for the size first, so the code can be drawn at
 * a whole number of pixels per module: LovyanGFX's qrcode() rounds the module
 * size down to fit the width it is given, which would leave a ragged margin.
 */
void drawQr(const char* text, int cy) {
  static uint8_t buffer[256];  // version 6 (41 x 41 modules) needs 211 bytes
  QRCode qr;
  uint8_t version = 1;
  for (; version <= 6; ++version) {
    if (lgfx_qrcode_getBufferSize(version) <= sizeof(buffer) &&
        lgfx_qrcode_initText(&qr, buffer, version, 0 /* ECC low */, text) == 0) {
      break;
    }
  }
  if (version > 6) {
    return;  // cannot happen for the short texts used here
  }
  const int code_px = qr.size * kQrModulePx;
  const int plate_px = code_px + 2 * kQrQuietModules * kQrModulePx;
  // A small corner radius: a larger one would bite into the quiet zone by
  // the finder squares.
  tft.fillRoundRect(kCenterX - plate_px / 2, cy - plate_px / 2, plate_px, plate_px, 5,
                    TFT_WHITE);
  tft.qrcode(text, kCenterX - code_px / 2, cy - code_px / 2, code_px, version);
}

void drawPortal() {
  char qr[96];
  util::setup::wifiQrText(config::kPortalApName, wifiSetupApPassword(), qr, sizeof(qr));
  clearSetup();
  line(services::location::isSet() ? "Wi-Fi setup" : "Step 1 of 2", 21, kSmall, kSetupStep);
  line("Scan to join Wi-Fi", 40, kBody, kSetupText);
  drawQr(qr, kQrCenterY);
  line("No camera? Tap the button", 193, kTiny, kSetupMuted);
  line("for the password", 208, kTiny, kSetupMuted);
}

void drawPortalText() {
  clearSetup();
  line("Join this Wi-Fi", 38, kSmall, kSetupStep);
  line("Network", 62, kTiny, kSetupMuted);
  line(config::kPortalApName, 81, kBody, kSetupText);
  line("Password", 106, kTiny, kSetupMuted);
  line(wifiSetupApPassword(), 136, kBig, kSetupText);
  line("A page opens on your phone", 174, kSmall, kSetupText);
  line("Tap again for the code", 200, kTiny, kSetupMuted);
}

void drawPortalPhone() {
  char no_page[40];
  snprintf(no_page, sizeof(no_page), "No page? %s", config::kPortalIp);
  clearSetup();
  line("Phone connected", 76, kTitle, kSetupGood);
  line("Pick your home Wi-Fi", 110, kBody, kSetupText);
  line("on your phone", 132, kBody, kSetupText);
  line(no_page, 166, kSmall, kSetupMuted);
  line("Only 2.4 GHz networks work", 192, kTiny, kSetupMuted);
}

void drawPortalRetrying() {
  clearSetup();
  line("Can't reach Wi-Fi", 68, kTitle, kSetupStep);
  line(s_arg_ssid, 98, kBody, kSetupText);
  if (s_arg_flag) {
    line("or its password changed", 120, kTiny, kSetupMuted);
  }
  line("Trying again every 3 min", 142, kSmall, kSetupMuted);
  line("New Wi-Fi? Tap the button", 172, kSmall, kSetupText);
  line("to set it up", 192, kSmall, kSetupText);
}

void drawJoinFailed() {
  clearSetup();
  switch (s_arg_why) {
    case util::setup::JoinFailure::kNotFound:
      line("Not found", 76, kTitle, kSetupBad);
      line(s_arg_ssid, 106, kBody, kSetupText);
      line("Use a 2.4 GHz network", 138, kBody, kSetupText);
      line("and try again on your phone", 166, kSmall, kSetupMuted);
      break;
    case util::setup::JoinFailure::kWrongPassword:
      line("Could not join", 72, kTitle, kSetupBad);
      line(s_arg_ssid, 102, kBody, kSetupText);
      line("Check the password", 134, kBody, kSetupText);
      line("and try again on your phone", 162, kSmall, kSetupMuted);
      break;
    default:
      line("Could not join", 76, kTitle, kSetupBad);
      line(s_arg_ssid, 106, kBody, kSetupText);
      line("Try again on your phone", 140, kBody, kSetupText);
      break;
  }
}

void drawLocationStep() {
  char url[40];
  snprintf(url, sizeof(url), "http://%s/settings", s_arg_text);
  char open[48];
  snprintf(open, sizeof(open), "or open %s", s_arg_text);
  clearSetup();
  line("Step 2 of 2", 24, kSmall, kSetupStep);
  line("Scan to set location", 44, kBody, kSetupText);
  drawQr(url, kQrCenterY);
  line(open, 190, kSmall, kSetupText);
  line("on the same Wi-Fi", 206, kTiny, kSetupMuted);
}

char s_info_version[40] = "";
char s_info_status[48] = "";

void drawInfo() {
  char url[40];
  snprintf(url, sizeof(url), "http://%s/settings", s_arg_text);
  clearSetup();
  line("Radar info", 24, kSmall, kSetupStep);
  line(s_info_version, 42, kTiny, kSetupMuted);
  drawQr(url, kQrCenterY);
  line(s_arg_text, 188, kSmall, kSetupText);
  line(s_info_status, 206, kTiny, kSetupMuted);
}

void drawLocationSaved() {
  clearSetup();
  line("Location saved", 88, kTitle, kSetupGood);
  line("Showing planes near", 122, kBody, kSetupMuted);
  line(s_arg_text, 148, kBody, kSetupText);
}

void drawWifiLost() {
  clearSetup();
  line("Wi-Fi lost", 66, kTitle, kSetupStep);
  line(s_arg_ssid, 96, kBody, kSetupText);
  line("Trying again...", 122, kSmall, kSetupMuted);
  line("New router? Hold the", 158, kSmall, kSetupText);
  line("button for 10 s", 178, kSmall, kSetupText);
  resetSpinner(ui::glassColor(kSetupBg), true);
  drawSpinnerDots();
}

void drawWifiReset() {
  clearSetup();
  line("Wi-Fi reset", 100, kTitle, kSetupStep);
  line("Restarting...", 134, kBody, kSetupMuted);
}

void drawErased() {
  clearSetup();
  line("All erased", 100, kTitle, kSetupStep);
  line("Restarting into setup...", 134, kBody, kSetupMuted);
}

/** Draw `fn` now, and remember it for after the hold overlay. */
void show(void (*fn)()) {
  s_redraw = fn;
  s_overlay_secs = 0;
  fn();
}

void copyArg(char* dst, size_t len, const char* src) {
  snprintf(dst, len, "%s", (src != nullptr && src[0] != '\0') ? src : "network");
}

}  // namespace

void statusScreenConnectingBegin(const char* ssid) {
  const char* name = (ssid != nullptr && ssid[0] != '\0') ? ssid : "network";
  strncpy(s_connecting_ssid, name, sizeof(s_connecting_ssid) - 1);
  s_connecting_ssid[sizeof(s_connecting_ssid) - 1] = '\0';
  fitSsidLine();
  resetSpinner(config::kColorBlack, false);
  s_connecting_text_drawn = false;
  s_redraw = nullptr;  // the animation redraws itself on the next tick
  s_overlay_secs = 0;
  drawConnectingText();
  drawSpinnerDots();
}

void statusScreenConnectingTick() {
  if (!s_connecting_text_drawn) {
    drawConnectingText();
  }
  tickSpinner();
}

void statusScreenPortal() { show(drawPortal); }

void statusScreenPortalText() { show(drawPortalText); }

void statusScreenPortalPhone() { show(drawPortalPhone); }

void statusScreenPortalRetrying(const char* ssid, bool password_suspect) {
  copyArg(s_arg_ssid, sizeof(s_arg_ssid), ssid);
  s_arg_flag = password_suspect;
  show(drawPortalRetrying);
}

void statusScreenJoinFailed(const char* ssid, util::setup::JoinFailure why) {
  copyArg(s_arg_ssid, sizeof(s_arg_ssid), ssid);
  s_arg_why = why;
  show(drawJoinFailed);
}

void statusScreenLocationStep(const char* ip) {
  snprintf(s_arg_text, sizeof(s_arg_text), "%s", ip != nullptr ? ip : "");
  show(drawLocationStep);
}

void statusScreenInfo(const char* ip, const char* version_line, const char* status_line) {
  snprintf(s_arg_text, sizeof(s_arg_text), "%s", ip != nullptr ? ip : "");
  snprintf(s_info_version, sizeof(s_info_version), "%s", version_line != nullptr ? version_line : "");
  snprintf(s_info_status, sizeof(s_info_status), "%s", status_line != nullptr ? status_line : "");
  show(drawInfo);
}

void statusScreenLocationSaved(const char* place) {
  snprintf(s_arg_text, sizeof(s_arg_text), "%s", place != nullptr ? place : "");
  show(drawLocationSaved);
}

void statusScreenWifiLost(const char* ssid) {
  copyArg(s_arg_ssid, sizeof(s_arg_ssid), ssid);
  show(drawWifiLost);
}

void statusScreenWifiLostTick() {
  if (s_redraw == drawWifiLost) {
    tickSpinner();
  }
}

void statusScreenConnectFailed() {
  show([]() {
    const TextLine lines[] = {
        {"Could not connect", 1.15f, &kGfxTitle},
        {"Check Wi-Fi password", 1.0f, &kGfxBody},
        {"and signal strength.", 1.0f, &kGfxBody},
        {"Hold BOOT 10 sec", 1.0f, &kGfxBody},
        {"to reset Wi-Fi", 1.0f, &kGfxBody},
    };
    drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                  sizeof(lines) / sizeof(lines[0]));
  });
}

void statusScreenWifiReset() { show(drawWifiReset); }

void statusScreenErased() { show(drawErased); }

void statusScreenHoldOverlay(unsigned long held_ms) {
  if (held_ms < config::kBootResetWarnMs) {
    if (s_overlay_secs != 0) {
      s_overlay_secs = 0;
      if (s_redraw != nullptr) {
        s_redraw();
      } else {
        s_connecting_text_drawn = false;  // the connecting animation repaints itself
      }
    }
    return;
  }
  const unsigned long left_ms =
      held_ms >= config::kBootResetHoldMs ? 0 : config::kBootResetHoldMs - held_ms;
  const unsigned long secs = (left_ms + 999) / 1000 + 1;  // +1 keeps 0 s distinct from "none"
  if (secs == s_overlay_secs) {
    return;
  }
  s_overlay_secs = secs;
  // The same box and words as the countdown on the radar (radar_display.cpp).
  const int x = 30, y = 92, w = 180, h = 56;
  tft.fillRoundRect(x, y, w, h, 10, ui::glassColor(ui::Rgb{40, 24, 0}));
  tft.drawRoundRect(x, y, w, h, 10, ui::glassColor(ui::Rgb{255, 190, 60}));
  applyStyle(kSmall);
  tft.setTextDatum(textdatum_t::top_center);
  tft.setTextColor(ui::glassColor(kSetupText), ui::glassColor(ui::Rgb{40, 24, 0}));
  tft.drawString("Keep holding to", x + w / 2, y + 7);
  tft.drawString("reset Wi-Fi", x + w / 2, y + 22);
  char text[32];
  snprintf(text, sizeof(text), "%lu s - release to cancel", secs - 1);
  applyStyle(kTiny);
  tft.setTextColor(ui::glassColor(ui::Rgb{255, 200, 90}), ui::glassColor(ui::Rgb{40, 24, 0}));
  tft.drawString(text, x + w / 2, y + 38);
  tft.setTextDatum(textdatum_t::middle_center);
}
