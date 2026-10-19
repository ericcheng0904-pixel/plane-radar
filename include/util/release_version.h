#pragma once

#include <cstdint>

/**
 * Release versions, as the over-the-air updater compares them (docs/OTA.md).
 *
 * scripts/version.py stamps every build with `git describe`: "v2.1.0" at a
 * release tag, "v2.1.0-3-g1a2b3c4", "...-dirty" or "...-private" anywhere
 * else. Only the first form is a release, and only a radar running a release
 * updates itself: a build someone flashed by hand to try something out must not
 * be replaced behind their back. scripts/make_release.py accepts the same
 * grammar, so a manifest the device would reject cannot be published.
 *
 * The offered version also becomes part of the firmware URL, so accepting
 * nothing but digits and dots here is what keeps a manifest from pointing the
 * download anywhere else. Free of Arduino headers so the host suite covers it.
 */
namespace util::release_version {

struct Version {
  uint16_t major;
  uint16_t minor;
  uint16_t patch;
};

/**
 * Parse a release: exactly "v<major>.<minor>.<patch>", each part decimal,
 * 0..65535 and without leading zeros ("v2.01.0" would compare equal to
 * "v2.1.0" yet name a different file). Anything before or after fails.
 */
bool parse(const char* text, Version& out);

/** Negative, zero or positive as `a` is older than, equal to or newer than `b`. */
int compare(const Version& a, const Version& b);

/**
 * Whether a radar running `running` should install `offered`: both are
 * releases and `offered` is strictly newer. Never a downgrade, never a
 * reinstall, and never anything for a development build.
 */
bool shouldInstall(const char* running, const char* offered);

}  // namespace util::release_version
