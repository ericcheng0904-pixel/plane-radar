/**
 * Host tests for what the over-the-air updater will rely on (docs/OTA.md):
 * which versions it installs, and that the embedded public key is either the
 * placeholder that refuses everything or a well-formed P-256 point.
 *
 * The same version strings are checked against scripts/ota_common.py by
 * tools/ota/test_release_tools.py, so the device and the release tool cannot
 * drift apart on what counts as a release.
 */
#include <unity.h>

#include <cstddef>
#include <cstdint>

#include "ota_public_key.h"
#include "util/release_version.h"

using util::release_version::Version;

void setUp() {}
void tearDown() {}

// --- parse ------------------------------------------------------------------

void test_parse_reads_a_release() {
  Version v{};
  TEST_ASSERT_TRUE(util::release_version::parse("v2.1.0", v));
  TEST_ASSERT_EQUAL_UINT16(2, v.major);
  TEST_ASSERT_EQUAL_UINT16(1, v.minor);
  TEST_ASSERT_EQUAL_UINT16(0, v.patch);
  TEST_ASSERT_TRUE(util::release_version::parse("v0.0.0", v));
  TEST_ASSERT_TRUE(util::release_version::parse("v65535.65535.65535", v));
  TEST_ASSERT_EQUAL_UINT16(65535, v.patch);
  TEST_ASSERT_TRUE(util::release_version::parse("v10.20.30", v));
  TEST_ASSERT_EQUAL_UINT16(20, v.minor);
}

/** Everything git describe makes between releases, and what a hostile manifest might send. */
void test_parse_rejects_everything_else() {
  const char* const bad[] = {
      "v2.1.0-dirty", "v2.1.0-3-g1a2b3c4", "v2.1.0-private", "1a2b3c4", "dev", "",
      "2.1.0", "V2.1.0", "v2.1", "v2.1.0.0", "v2..0", "v.1.0", "v2.1.", "v02.1.0",
      "v2.01.0", "v2.1.00", "v65536.0.0", "v999999.0.0", "v2.1.0 ", " v2.1.0",
      "v2.1.0/../x", "v2.1.-1", "v+2.1.0", "v2.1.0\n",
  };
  for (const char* text : bad) {
    Version v{7, 7, 7};
    TEST_ASSERT_FALSE_MESSAGE(util::release_version::parse(text, v), text);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(7, v.major, text);  // untouched on failure
  }
  Version v{};
  TEST_ASSERT_FALSE(util::release_version::parse(nullptr, v));
}

// --- compare / shouldInstall --------------------------------------------------

void test_compare_orders_by_major_minor_patch() {
  TEST_ASSERT_EQUAL_INT(0, util::release_version::compare({2, 1, 0}, {2, 1, 0}));
  TEST_ASSERT_LESS_THAN_INT(0, util::release_version::compare({2, 1, 0}, {2, 1, 1}));
  TEST_ASSERT_LESS_THAN_INT(0, util::release_version::compare({2, 9, 9}, {3, 0, 0}));
  TEST_ASSERT_GREATER_THAN_INT(0, util::release_version::compare({2, 10, 0}, {2, 9, 0}));
  TEST_ASSERT_GREATER_THAN_INT(0, util::release_version::compare({3, 0, 0}, {2, 65535, 65535}));
}

void test_should_install_only_a_newer_release() {
  TEST_ASSERT_TRUE(util::release_version::shouldInstall("v2.1.0", "v2.1.1"));
  TEST_ASSERT_TRUE(util::release_version::shouldInstall("v2.1.0", "v2.2.0"));
  TEST_ASSERT_TRUE(util::release_version::shouldInstall("v2.9.0", "v2.10.0"));  // not string order
  TEST_ASSERT_TRUE(util::release_version::shouldInstall("v2.1.0", "v3.0.0"));
  TEST_ASSERT_FALSE(util::release_version::shouldInstall("v2.1.0", "v2.1.0"));  // no reinstall
  TEST_ASSERT_FALSE(util::release_version::shouldInstall("v2.1.1", "v2.1.0"));  // no downgrade
  TEST_ASSERT_FALSE(util::release_version::shouldInstall("v3.0.0", "v2.99.99"));
}

/** A build flashed by hand to try something is never replaced behind its owner's back. */
void test_development_builds_never_update() {
  const char* const dev[] = {"dev", "1a2b3c4", "v2.1.0-dirty", "v2.1.0-3-g1a2b3c4",
                             "1a2b3c4-dirty-private", "v2.1.0-private"};
  for (const char* running : dev) {
    TEST_ASSERT_FALSE_MESSAGE(util::release_version::shouldInstall(running, "v9.0.0"), running);
  }
  TEST_ASSERT_FALSE(util::release_version::shouldInstall(nullptr, "v9.0.0"));
}

void test_an_unparseable_offer_is_refused() {
  TEST_ASSERT_FALSE(util::release_version::shouldInstall("v2.1.0", "v2.2.0-dirty"));
  TEST_ASSERT_FALSE(util::release_version::shouldInstall("v2.1.0", "../v9.0.0"));
  TEST_ASSERT_FALSE(util::release_version::shouldInstall("v2.1.0", ""));
  TEST_ASSERT_FALSE(util::release_version::shouldInstall("v2.1.0", nullptr));
}

// --- embedded public key ------------------------------------------------------

/**
 * The placeholder must be unusable as a key (all zero, not the 0x04 prefix of
 * an uncompressed point) so that even an updater that forgot the flag cannot
 * verify against it; a real key must have the shape mbedTLS expects.
 */
void test_public_key_is_placeholder_or_a_p256_point() {
  TEST_ASSERT_EQUAL_size_t(65, sizeof(config::kOtaPublicKey));
  size_t nonzero = 0;
  for (uint8_t b : config::kOtaPublicKey) {
    nonzero += b != 0 ? 1 : 0;
  }
  if (config::kOtaPublicKeyIsPlaceholder) {
    TEST_ASSERT_EQUAL_size_t(0, nonzero);
    TEST_ASSERT_EQUAL_STRING("placeholder", config::kOtaPublicKeyFingerprint);
  } else {
    TEST_ASSERT_EQUAL_HEX8(0x04, config::kOtaPublicKey[0]);
    TEST_ASSERT_GREATER_THAN_size_t(32, nonzero);
    TEST_ASSERT_EQUAL_size_t(16, sizeof(config::kOtaPublicKeyFingerprint) - 1);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_reads_a_release);
  RUN_TEST(test_parse_rejects_everything_else);
  RUN_TEST(test_compare_orders_by_major_minor_patch);
  RUN_TEST(test_should_install_only_a_newer_release);
  RUN_TEST(test_development_builds_never_update);
  RUN_TEST(test_an_unparseable_offer_is_refused);
  RUN_TEST(test_public_key_is_placeholder_or_a_p256_point);
  return UNITY_END();
}
