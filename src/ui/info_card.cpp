#include "ui/info_card.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "data/aircraft_types.h"
#include "data/airline_brands.h"
#include "data/airline_logos.h"
#include "hardware/display_font.h"
#include "ui/aircraft_glyph.h"
#include "ui/blend.h"
#include "ui/panel_color.h"
#include "ui/radar_range.h"
#include "services/routes.h"
#include "ui/theme.h"
#include "util/aircraft_class.h"
#include "util/airline_badge.h"
#include "util/format.h"

namespace ui::card {

namespace {

// Geometry. The disc is 240 px across; the card is the widest rounded
// rectangle whose corners stay on the glass at either position.
constexpr int kX = 44;
constexpr int kW = 152;
constexpr int kH = 52;
/** With a route line the card is taller and starts higher up. */
constexpr int kHTall = 64;
constexpr int kRadius = 9;
constexpr int kYBottom = 158;
constexpr int kYTop = 240 - kYBottom - kH;
constexpr int kYBottomTall = 148;
constexpr int kYTopTall = 240 - kYBottomTall - kHTall;
constexpr int kPad = 6;
constexpr int kLogo = 32;
static_assert(kLogo == data::airline_logos::kSize, "logo slot must fit the logos");
constexpr int kBadgeRadius = 7;
constexpr int kTextX = kX + kPad + kLogo + 6;
constexpr int kTextW = kX + kW - kPad - kTextX;

constexpr size_t kRotateAmong = 3;
constexpr unsigned long kDwellMs = 6000;

// Colours as seen on the glass. kCardBg must match CARD_BG in
// scripts/build_airline_brands.py, which pre-blends (private) logo edges onto it.
constexpr Rgb kCardBg{14, 18, 30};
constexpr Rgb kName{255, 255, 255};
constexpr Rgb kFlight{150, 200, 255};
constexpr Rgb kType{175, 188, 208};
/** The kind tag after the type ("WB 4-eng"): the symbol's key, a step quieter. */
constexpr Rgb kKind{120, 170, 220};
constexpr Rgb kSpeed{200, 210, 225};
constexpr Rgb kClimb{110, 230, 120};
constexpr Rgb kDescend{255, 150, 70};
constexpr Rgb kBadge{40, 52, 76};
/** Share of the radar that shows through the card (0..256). */
constexpr int kShowThrough = 22;
/** Below this |vertical rate| the aircraft counts as level. */
constexpr int kLevelFpm = 300;

char s_hex[7] = "";
unsigned long s_since_ms = 0;
/** Aircraft featured so far in the current round of the rotation. */
char s_shown[kRotateAmong][7] = {};
size_t s_shown_next = 0;

/** Horizontal inset of a rounded rectangle's edge at row `yy`. */
int cornerInset(int y, int h, int r, int yy) {
  int dy = 0;
  if (yy < y + r) {
    dy = (y + r) - yy;
  } else if (yy > y + h - 1 - r) {
    dy = yy - (y + h - 1 - r);
  } else {
    return 0;
  }
  dy = std::min(dy, r);
  return r - static_cast<int>(lroundf(sqrtf(static_cast<float>(r * r - dy * dy))));
}

/**
 * Frosted glass: blend every pixel inside the rounded rectangle toward
 * kCardBg, straight in the sprite buffer (big-endian RGB565). Blending per
 * channel is the same in either R/B order, so the panel swap needs no care.
 */
void frost(LGFX_Sprite& f, int x, int y, int w, int h, int r) {
  int row0 = 0;
  int row1 = 0;
  clipRows(f, &row0, &row1);
  const uint16_t tint = glassColor(kCardBg);
  for (int yy = std::max(row0, y); yy < std::min(row1, y + h); ++yy) {
    const int inset = cornerInset(y, h, r, yy);
    blendSpanClipped(f, yy, x + inset, x + w - inset, tint, 256 - kShowThrough);
  }
}

void useFont(LGFX_Sprite& f, size_t index) {
  if (displayFontIsSmooth()) {
    displayFontEnsureLoaded(f, index);
  } else {
    displayFontSetBitmap(f, &fonts::FreeSans9pt7b);
  }
}

/** Cut `s` until it fits `max_w` pixels in the current font. */
void fit(LGFX_Sprite& f, char* s, int max_w) {
  size_t n = strlen(s);
  while (n > 1 && f.textWidth(s) > max_w) {
    s[--n] = '\0';
  }
}

/** Text drawn with only a foreground colour blends into what is below it. */
void text(LGFX_Sprite& f, const char* s, int x, int y, const Rgb& c,
          textdatum_t datum = textdatum_t::top_left);

/**
 * After the type name, what kind of aircraft the symbol shows: "RJ", "NB",
 * "WB", "TP", "BJ", with the engine count when it is not two ("WB 4-eng").
 * Only what fits in the rest of the line; nothing for kinds without a tag.
 */
void drawKind(LGFX_Sprite& f, const services::adsb::Aircraft& plane, int x, int y) {
  const char* kind = radar::showClassIcons()
                         ? util::aircraft::shortLabel(static_cast<util::aircraft::Klass>(plane.klass))
                         : nullptr;
  if (kind == nullptr) {
    return;
  }
  constexpr int kGap = 6;
  const int room = kTextX + kTextW - x - kGap;
  char tag[12];
  if (plane.engines != 0 && plane.engines != 2) {
    snprintf(tag, sizeof(tag), "%s %u-eng", kind, static_cast<unsigned>(plane.engines));
    if (f.textWidth(tag) <= room) {
      text(f, tag, x + kGap, y, kKind);
      return;
    }
  }
  if (f.textWidth(kind) <= room) {
    text(f, kind, x + kGap, y, kKind);
  }
}

void text(LGFX_Sprite& f, const char* s, int x, int y, const Rgb& c, textdatum_t datum) {
  f.setTextDatum(datum);
  f.setTextColor(glassColor(c));
  f.drawString(s, x, y);
}

/** Up or down triangle for climb / descent, centred on (cx, cy). */
void drawTrend(LGFX_Sprite& f, int cx, int cy, int vrate_fpm) {
  if (vrate_fpm > kLevelFpm) {
    f.fillTriangle(cx, cy - 4, cx - 4, cy + 3, cx + 4, cy + 3, glassColor(kClimb));
  } else if (vrate_fpm < -kLevelFpm) {
    f.fillTriangle(cx, cy + 4, cx - 4, cy - 3, cx + 4, cy - 3, glassColor(kDescend));
  }
}

/** The airline's brand colour as 0xRRGGBB, or the neutral badge colour. */
uint32_t badgeColor(const data::airline_brands::AirlineBrand& brand) {
  if (brand.color != data::airline_brands::kNoColor) {
    return brand.color;
  }
  return (static_cast<uint32_t>(kBadge.r) << 16) | (static_cast<uint32_t>(kBadge.g) << 8) |
         kBadge.b;
}

Rgb unpack(uint32_t rgb) {
  return Rgb{static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
             static_cast<uint8_t>(rgb)};
}

/**
 * The 32 px slot left of the text. An airline flight gets its logo when this
 * build has one, else a badge in its brand colour (the code on it is drawn
 * later, by drawBadgeCode); anything else gets its own silhouette. True when
 * the badge still needs its code.
 */
bool drawLogoSlot(LGFX_Sprite& f, int x, int y,
                  const data::airline_brands::AirlineBrand* brand,
                  const services::adsb::Aircraft& plane) {
  if (brand != nullptr) {
    const uint16_t* logo = data::airline_logos::logo32(*brand);
    if (logo != nullptr) {
      f.pushImage(x, y, kLogo, kLogo, logo, data::airline_logos::kTransparent);
      return false;
    }
    const Rgb fill = unpack(badgeColor(*brand));
    f.fillRoundRect(x, y, kLogo, kLogo, kBadgeRadius, glassColor(fill));
    // A lighter rim keeps navy and black badges from dissolving into the card.
    f.drawRoundRect(x, y, kLogo, kLogo, kBadgeRadius,
                    glassColor(mix(fill, Rgb{255, 255, 255}, 0.3f)));
    return true;
  }
  // Not an airline flight: a badge with the aircraft's own silhouette, nose up.
  f.fillRoundRect(x, y, kLogo, kLogo, kBadgeRadius, glassColor(kBadge));
  const uint16_t fill = glassColor(glyph::altitudeColor(plane.alt_ft));
  const uint8_t klass = radar::showClassIcons()
                            ? plane.klass
                            : static_cast<uint8_t>(util::aircraft::Klass::kJet);
  // As large as fits inside the badge with a 3 px margin, at most 1.5x map size.
  const float scale =
      std::min(1.5f, (kLogo / 2 - 3) / static_cast<float>(glyph::radiusPx(klass, 1.0f)));
  glyph::draw(f, x + kLogo / 2, y + kLogo / 2, 0.0f, klass, scale, fill, fill, plane.engines,
              plane.rear_engines);
  return false;
}

/**
 * The airline's code ("UA", or "AMF" without an IATA code), centred on the
 * badge drawLogoSlot filled. Called while line 1's 13 px font is loaded: every
 * font switch re-reads a VLW glyph table onto the heap, so the badge borrows
 * the fonts the card loads anyway instead of adding one. A code too wide for
 * 13 px drops to 11 px, which line 2 loads next regardless.
 */
void drawBadgeCode(LGFX_Sprite& f, int x, int y,
                   const data::airline_brands::AirlineBrand& brand) {
  const char* code = util::airline_badge::code(brand.iata, brand.icao);
  if (f.textWidth(code) > kLogo - 2) {
    useFont(f, kUiFont11);
  }
  const uint32_t ink = util::airline_badge::inkOn(badgeColor(brand));
  text(f, code, x + kLogo / 2, y + kLogo / 2, unpack(ink), textdatum_t::middle_center);
}

}  // namespace

int choose(const services::adsb::Aircraft* planes, const size_t* nearest,
           size_t count, unsigned long now_ms) {
  if (count == 0) {
    s_hex[0] = '\0';
    return -1;
  }
  const size_t pool = std::min(count, kRotateAmong);
  int current = -1;
  for (size_t k = 0; k < pool; ++k) {
    if (s_hex[0] != '\0' && strcmp(planes[nearest[k]].hex, s_hex) == 0) {
      current = static_cast<int>(k);
      break;
    }
  }
  if (current >= 0 && (pool == 1 || now_ms - s_since_ms < kDwellMs)) {
    return static_cast<int>(nearest[current]);
  }
  // Next: the nearest one not shown yet this round, so a pair swapping places
  // between turns cannot keep the third off the card. A full round starts over.
  auto shown = [&](const char* hex) {
    for (const auto& h : s_shown) {
      if (h[0] != '\0' && strcmp(h, hex) == 0) {
        return true;
      }
    }
    return false;
  };
  size_t next = pool;
  for (size_t k = 0; k < pool && next == pool; ++k) {
    if (!shown(planes[nearest[k]].hex)) {
      next = k;
    }
  }
  if (next == pool) {
    for (auto& h : s_shown) {
      h[0] = '\0';
    }
    next = (current < 0 ? 0 : (static_cast<size_t>(current) + 1) % pool);
  }
  const char* hex = planes[nearest[next]].hex;
  strncpy(s_hex, hex, sizeof(s_hex) - 1);
  s_hex[sizeof(s_hex) - 1] = '\0';
  strncpy(s_shown[s_shown_next], hex, sizeof(s_shown[0]) - 1);
  s_shown[s_shown_next][sizeof(s_shown[0]) - 1] = '\0';
  s_shown_next = (s_shown_next + 1) % kRotateAmong;
  s_since_ms = now_ms;
  return static_cast<int>(nearest[next]);
}

Rect rect(bool at_top, bool tall) {
  if (tall) {
    return Rect{kX, at_top ? kYTopTall : kYBottomTall, kW, kHTall};
  }
  return Rect{kX, at_top ? kYTop : kYBottom, kW, kH};
}

namespace {

/** Small right-pointing arrow centred on (cx, cy). */
void drawArrow(LGFX_Sprite& f, int cx, int cy, uint16_t color) {
  f.drawFastHLine(cx - 4, cy, 6, color);
  f.fillTriangle(cx + 4, cy, cx + 1, cy - 3, cx + 1, cy + 3, color);
}

/**
 * The route as IATA codes: "MCO -> ORD"; a multi-stop chain whose current leg
 * is unclear as text ("ORD-CHS-ORD"); or one end when only that is known:
 * "-> ORD" arriving, "ORD ->" departing.
 */
void drawRoute(LGFX_Sprite& f, const services::routes::Shown& shown, int x, int y) {
  using Kind = services::routes::Shown::Kind;
  const uint16_t color = glassColor(theme::current().tag_route);
  useFont(f, kUiFont11);
  char from[8] = "";
  char to[8] = "";
  const char* codes = shown.codes;
  if (shown.kind == Kind::kRoute && strchr(codes, '-') != strrchr(codes, '-')) {
    // Several stops and no telling which leg: the whole chain, as text.
    f.setTextDatum(textdatum_t::top_left);
    f.setTextColor(color);
    f.drawString(codes, x, y);
    return;
  }
  if (shown.kind == Kind::kRoute) {
    const char* last = strrchr(codes, '-');
    snprintf(from, sizeof(from), "%.*s", static_cast<int>(strcspn(codes, "-")), codes);
    snprintf(to, sizeof(to), "%s", last != nullptr ? last + 1 : "");
  } else if (shown.kind == Kind::kArriving) {
    snprintf(to, sizeof(to), "%s", codes);
  } else {
    snprintf(from, sizeof(from), "%s", codes);
  }
  constexpr int kArrowW = 14;
  f.setTextDatum(textdatum_t::top_left);
  f.setTextColor(color);
  f.drawString(from, x, y);
  const int from_w = from[0] != '\0' ? f.textWidth(from) : 0;
  drawArrow(f, x + from_w + kArrowW / 2, y + f.fontHeight() / 2, color);
  f.drawString(to, x + from_w + kArrowW, y);
}

}  // namespace

void draw(LGFX_Sprite& f, const services::adsb::Aircraft& plane, bool at_top,
          bool miles, const services::routes::Shown* route) {
  const bool tall = route != nullptr;
  const Rect box = rect(at_top, tall);
  const int y = box.y;
  frost(f, box.x, box.y, box.w, box.h, kRadius);
  f.drawRoundRect(box.x, box.y, box.w, box.h, kRadius,
                  glassColor(theme::current().card_border));

  const data::airline_brands::AirlineBrand* brand =
      data::airline_brands::brandForCallsign(plane.callsign);

  const int logo_x = kX + kPad;
  const int logo_y = y + kPad;
  const bool badge_code = drawLogoSlot(f, logo_x, logo_y, brand, plane);

  // Flight number under the logo, like a boarding pass.
  char flight[12];
  util::format::flightNumber(plane.callsign, brand != nullptr ? brand->iata : nullptr,
                             flight, sizeof(flight));
  useFont(f, kUiFont9);
  fit(f, flight, kLogo + 6);
  text(f, flight, logo_x + kLogo / 2, logo_y + kLogo + 2, kFlight,
       textdatum_t::top_center);

  // Line 1: who. Airline name, else the callsign / registration.
  char line[24];
  snprintf(line, sizeof(line), "%s", brand != nullptr ? brand->name : plane.callsign);
  useFont(f, kUiFont13);
  fit(f, line, kTextW);
  text(f, line, kTextX, y + 5, kName);
  if (badge_code && brand != nullptr) {
    drawBadgeCode(f, logo_x, logo_y, *brand);
  }

  // Line 2: what.
  const char* type_name = data::aircraft_types::nameFor(plane.type);
  snprintf(line, sizeof(line), "%s", type_name != nullptr ? type_name : plane.type);
  // Long names ("Bombardier CRJ700") drop to the small font before being cut.
  useFont(f, kUiFont11);
  if (f.textWidth(line) > kTextW) {
    useFont(f, kUiFont9);
  }
  fit(f, line, kTextW);
  const int type_y = tall ? y + 35 : y + 23;
  if (tall) {
    drawRoute(f, *route, kTextX, y + 21);
    useFont(f, kUiFont11);
    if (f.textWidth(line) > kTextW) {
      useFont(f, kUiFont9);
    }
    fit(f, line, kTextW);
  }
  text(f, line, kTextX, type_y, kType);
  drawKind(f, plane, kTextX + f.textWidth(line), type_y);
  useFont(f, kUiFont11);

  // Line 3: how high, which way, how fast.
  int x = kTextX;
  const int line3_y = tall ? y + 49 : y + 36;
  if (plane.alt_ft != services::adsb::kAltUnknown) {
    char alt[16];
    util::format::thousands(plane.alt_ft, alt, sizeof(alt));
    strncat(alt, " ft", sizeof(alt) - strlen(alt) - 1);
    text(f, alt, x, line3_y, glyph::altitudeColor(plane.alt_ft));
    x += f.textWidth(alt) + 4;
    if (plane.vrate_fpm > kLevelFpm || plane.vrate_fpm < -kLevelFpm) {
      drawTrend(f, x + 4, line3_y + f.fontHeight() / 2, plane.vrate_fpm);
      x += 12;
    }
  }
  if (plane.gs_knots > 0.0f) {
    const float speed = miles ? plane.gs_knots * 1.15078f : plane.gs_knots * 1.852f;
    char spd[12];
    snprintf(spd, sizeof(spd), "%d %s", static_cast<int>(lroundf(speed)),
             miles ? "mph" : "km/h");
    if (x + f.textWidth(spd) <= kTextX + kTextW) {
      text(f, spd, x, line3_y, kSpeed);
    }
  }
  f.setTextDatum(textdatum_t::top_left);
}

void reset() {
  s_hex[0] = '\0';
  s_since_ms = 0;
  for (auto& h : s_shown) {
    h[0] = '\0';
  }
  s_shown_next = 0;
}

}  // namespace ui::card
