/**
 * Host tests for the logo-free airline badge and the brand and logo lookups.
 *
 * Published builds carry no airline logos (they are trademarks), so the card's
 * badge — the airline's code on its brand colour — is what most people see.
 * These check that the badge text is always legible, that the brand table the
 * generator writes is well formed, and that flight numbers and airline names
 * come out as they did when the table still carried logos.
 */
#include <unity.h>

#include <vector>

#include <cstdint>
#include <cstring>

#include "data/airline_brands.h"
#include "util/logo_pack.h"
#include "util/airline_badge.h"
#include "util/format.h"
#include "util/icao_table.h"

// platformio.ini builds only src/util/ for the host. The brand table, its
// lookup and the logo seam are plain C++ with no Arduino headers, so they are
// compiled into this test directly instead.
#include "../../src/data/airline_brands.cpp"
#include "../../src/data/airline_brands_table.cpp"

using data::airline_brands::AirlineBrand;
using data::airline_brands::brandForCallsign;
using data::airline_brands::kBrandCount;
using data::airline_brands::kBrands;
using data::airline_brands::kNoColor;
namespace badge = util::airline_badge;

void setUp() {}
void tearDown() {}

/** Lowest contrast the better of the two inks can give on any fill. */
constexpr float kMinInkContrast = 4.3f;

// --- util::icao::find -------------------------------------------------------

struct Row {
  char icao[4];
  int value;
};

const Row kRows[] = {{"AAL", 1}, {"DAL", 2}, {"SWA", 3}, {"UAL", 4}};
constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

void test_find_hits_every_entry() {
  for (size_t i = 0; i < kRowCount; ++i) {
    const Row* row = util::icao::find(kRows, kRowCount, kRows[i].icao);
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_EQUAL_INT(kRows[i].value, row->value);
  }
}

void test_find_misses_between_and_beyond() {
  TEST_ASSERT_NULL(util::icao::find(kRows, kRowCount, "AAA"));  // before the first
  TEST_ASSERT_NULL(util::icao::find(kRows, kRowCount, "BAW"));  // between
  TEST_ASSERT_NULL(util::icao::find(kRows, kRowCount, "ZZZ"));  // after the last
  // A longer code must not match its three-letter start.
  TEST_ASSERT_NULL(util::icao::find(kRows, kRowCount, "UALX"));
  TEST_ASSERT_NULL(util::icao::find(kRows, kRowCount, ""));
}

void test_find_survives_empty_and_null() {
  TEST_ASSERT_NULL(util::icao::find(kRows, 0, "AAL"));
  TEST_ASSERT_NULL(util::icao::find(kRows, kRowCount, nullptr));
  TEST_ASSERT_NULL(util::icao::find(static_cast<const Row*>(nullptr), 4, "AAL"));
}

// --- util::airline_badge ----------------------------------------------------

void test_badge_code_prefers_iata() {
  TEST_ASSERT_EQUAL_STRING("UA", badge::code("UA", "UAL"));
  TEST_ASSERT_EQUAL_STRING("9E", badge::code("9E", "EDV"));
}

void test_badge_code_falls_back_to_icao() {
  TEST_ASSERT_EQUAL_STRING("AMF", badge::code("", "AMF"));
  TEST_ASSERT_EQUAL_STRING("AMF", badge::code(nullptr, "AMF"));
  TEST_ASSERT_EQUAL_STRING("", badge::code(nullptr, nullptr));
}

void test_contrast_ratio_matches_wcag() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 21.0f, badge::contrastRatio(0x000000, 0xFFFFFF));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 21.0f, badge::contrastRatio(0xFFFFFF, 0x000000));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, badge::contrastRatio(0x336699, 0x336699));
  // WCAG's own example: #777777 on white is 4.48:1.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.48f, badge::contrastRatio(0x777777, 0xFFFFFF));
}

void test_ink_is_dark_on_light_fills() {
  TEST_ASSERT_EQUAL_HEX32(badge::kDarkInk, badge::inkOn(0xFFE600));  // yellow
  TEST_ASSERT_EQUAL_HEX32(badge::kDarkInk, badge::inkOn(0xFFFFFF));
  TEST_ASSERT_EQUAL_HEX32(badge::kDarkInk, badge::inkOn(0x70CBF4));  // light blue
}

void test_ink_is_light_on_dark_fills() {
  TEST_ASSERT_EQUAL_HEX32(badge::kLightInk, badge::inkOn(0x002157));  // navy
  TEST_ASSERT_EQUAL_HEX32(badge::kLightInk, badge::inkOn(0x000000));
  TEST_ASSERT_EQUAL_HEX32(badge::kLightInk, badge::inkOn(0xC01933));  // red
  TEST_ASSERT_EQUAL_HEX32(badge::kLightInk, badge::inkOn(0x28344C));  // neutral badge
}

void test_ink_is_legible_on_every_fill() {
  for (uint32_t r = 0; r <= 0xFF; r += 0x11) {
    for (uint32_t g = 0; g <= 0xFF; g += 0x11) {
      for (uint32_t b = 0; b <= 0xFF; b += 0x11) {
        const uint32_t fill = (r << 16) | (g << 8) | b;
        TEST_ASSERT_TRUE(badge::contrastRatio(fill, badge::inkOn(fill)) >= kMinInkContrast);
      }
    }
  }
}

// --- the generated brand table ----------------------------------------------

void test_brand_table_is_sorted_and_well_formed() {
  for (size_t i = 0; i < kBrandCount; ++i) {
    const AirlineBrand& b = kBrands[i];
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(strlen(b.icao)));
    for (int k = 0; k < 3; ++k) {
      TEST_ASSERT_TRUE(b.icao[k] >= 'A' && b.icao[k] <= 'Z');
    }
    if (i > 0) {
      // brandForCallsign binary-searches the table.
      TEST_ASSERT_TRUE(strcmp(kBrands[i - 1].icao, b.icao) < 0);
    }
    const size_t iata_len = strlen(b.iata);
    TEST_ASSERT_TRUE(iata_len == 0 || iata_len == 2);
    TEST_ASSERT_NOT_NULL(b.name);
    TEST_ASSERT_TRUE(strlen(b.name) > 0);
    TEST_ASSERT_TRUE(strlen(b.name) <= data::airline_brands::kMaxNameLen);
    TEST_ASSERT_TRUE(b.color == kNoColor || b.color <= 0xFFFFFF);
  }
}

void test_every_brand_badge_is_legible() {
  for (size_t i = 0; i < kBrandCount; ++i) {
    if (kBrands[i].color == kNoColor) {
      continue;
    }
    const uint32_t fill = kBrands[i].color;
    TEST_ASSERT_TRUE_MESSAGE(badge::contrastRatio(fill, badge::inkOn(fill)) >= kMinInkContrast,
                             kBrands[i].icao);
  }
}

void test_brand_lookup_names_and_codes() {
  struct Case {
    const char* callsign;
    const char* name;
    const char* iata;
  };
  const Case cases[] = {
      {"UAL1531", "United", "UA"},     {"SWA1966", "Southwest", "WN"},
      {"AAL100", "American", "AA"},    {"DLH4AB", "Lufthansa", "LH"},
      {"EDV5012", "Endeavor", "9E"},   {"ual1531", "United", "UA"},
      {"AMF1823", "Ameriflight", ""},
  };
  for (const Case& c : cases) {
    const AirlineBrand* b = brandForCallsign(c.callsign);
    TEST_ASSERT_NOT_NULL_MESSAGE(b, c.callsign);
    TEST_ASSERT_EQUAL_STRING(c.name, b->name);
    TEST_ASSERT_EQUAL_STRING(c.iata, b->iata);
  }
}

void test_brand_lookup_rejects_non_airlines() {
  TEST_ASSERT_NULL(brandForCallsign("N123AB"));   // tail number
  TEST_ASSERT_NULL(brandForCallsign("DEABC"));    // registration
  TEST_ASSERT_NULL(brandForCallsign("ZZZ123"));   // airline shape, unknown code
  TEST_ASSERT_NULL(brandForCallsign(""));
  TEST_ASSERT_NULL(brandForCallsign(nullptr));
}

void test_flight_numbers_use_the_iata_code() {
  char out[12];
  util::format::flightNumber("SWA1966", brandForCallsign("SWA1966")->iata, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("WN1966", out);
  util::format::flightNumber("UAL1531", brandForCallsign("UAL1531")->iata, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("UA1531", out);
  // No IATA code: the callsign stays as received.
  util::format::flightNumber("AMF1823", brandForCallsign("AMF1823")->iata, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("AMF1823", out);
}

// --- the logo pack ------------------------------------------------------------

namespace {
/** A pack as the generator writes it, for two 2x2 logos. */
std::vector<uint8_t> makePack(uint16_t size = 2, uint16_t key = 0x1FF8) {
  const char* codes[] = {"AAL", "UAL"};
  std::vector<uint8_t> body;
  const uint32_t first = 16 + 8 * 2;
  for (int n = 0; n < 2; ++n) {
    for (int i = 0; i < 4; ++i) body.push_back(i < 3 ? codes[n][i] : 0);
    const uint32_t off = first + n * size * size * 2;
    for (int i = 0; i < 4; ++i) body.push_back(static_cast<uint8_t>(off >> (8 * i)));
  }
  for (int n = 0; n < 2 * size * size; ++n) {
    body.push_back(static_cast<uint8_t>(n));
    body.push_back(0);
  }
  const uint32_t crc = util::logo_pack::crc32(0, body.data(), body.size());
  std::vector<uint8_t> pack = {'P', 'R', 'L', 'G', 1, 0, 2, 0,
                               static_cast<uint8_t>(size), 0,
                               static_cast<uint8_t>(key), static_cast<uint8_t>(key >> 8)};
  for (int i = 0; i < 4; ++i) pack.push_back(static_cast<uint8_t>(crc >> (8 * i)));
  pack.insert(pack.end(), body.begin(), body.end());
  return pack;
}
}  // namespace

void test_logo_pack_crc_matches_zlib() {
  const uint8_t text[] = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926, util::logo_pack::crc32(0, text, 9));
}

void test_logo_pack_header_and_lookup() {
  const auto pack = makePack();
  util::logo_pack::Header h{};
  TEST_ASSERT_TRUE(util::logo_pack::parseHeader(pack.data(), pack.size(), 2, 0x1FF8, &h));
  TEST_ASSERT_EQUAL_UINT16(2, h.count);
  TEST_ASSERT_EQUAL_HEX32(util::logo_pack::crc32(0, pack.data() + 16, pack.size() - 16), h.crc);
  uint32_t off = 0;
  TEST_ASSERT_TRUE(util::logo_pack::find(pack.data() + 16, 2, "UAL", pack.size(), 8, &off));
  TEST_ASSERT_EQUAL_UINT32(16 + 16 + 8, off);
  TEST_ASSERT_FALSE(util::logo_pack::find(pack.data() + 16, 2, "DAL", pack.size(), 8, &off));
  TEST_ASSERT_FALSE(util::logo_pack::find(pack.data() + 16, 2, nullptr, pack.size(), 8, &off));
}

void test_logo_pack_rejects_what_it_cannot_draw() {
  auto pack = makePack();
  util::logo_pack::Header h{};
  TEST_ASSERT_FALSE(util::logo_pack::parseHeader(pack.data(), pack.size(), 32, 0x1FF8, &h));
  TEST_ASSERT_FALSE(util::logo_pack::parseHeader(pack.data(), pack.size(), 2, 0xF81F, &h));
  TEST_ASSERT_FALSE(util::logo_pack::parseHeader(pack.data(), pack.size() - 1, 2, 0x1FF8, &h));
  std::vector<uint8_t> erased(16, 0xFF);  // a partition never written
  TEST_ASSERT_FALSE(util::logo_pack::parseHeader(erased.data(), 0x60000, 2, 0x1FF8, &h));
  pack[4] = 2;  // a newer format
  TEST_ASSERT_FALSE(util::logo_pack::parseHeader(pack.data(), pack.size(), 2, 0x1FF8, &h));
}

void test_logo_pack_index_cannot_point_outside() {
  auto pack = makePack();
  pack[16 + 8 + 4 + 3] = 0x7F;  // UAL's offset far past the end
  uint32_t off = 0;
  TEST_ASSERT_FALSE(util::logo_pack::find(pack.data() + 16, 2, "UAL", pack.size(), 8, &off));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_find_hits_every_entry);
  RUN_TEST(test_find_misses_between_and_beyond);
  RUN_TEST(test_find_survives_empty_and_null);
  RUN_TEST(test_badge_code_prefers_iata);
  RUN_TEST(test_badge_code_falls_back_to_icao);
  RUN_TEST(test_contrast_ratio_matches_wcag);
  RUN_TEST(test_ink_is_dark_on_light_fills);
  RUN_TEST(test_ink_is_light_on_dark_fills);
  RUN_TEST(test_ink_is_legible_on_every_fill);
  RUN_TEST(test_brand_table_is_sorted_and_well_formed);
  RUN_TEST(test_every_brand_badge_is_legible);
  RUN_TEST(test_brand_lookup_names_and_codes);
  RUN_TEST(test_brand_lookup_rejects_non_airlines);
  RUN_TEST(test_flight_numbers_use_the_iata_code);
  RUN_TEST(test_logo_pack_crc_matches_zlib);
  RUN_TEST(test_logo_pack_header_and_lookup);
  RUN_TEST(test_logo_pack_rejects_what_it_cannot_draw);
  RUN_TEST(test_logo_pack_index_cannot_point_outside);
  return UNITY_END();
}
