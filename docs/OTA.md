# Over-the-air updates

A radar given away has to keep working for years with nobody at a USB port.
It has to take fixes by itself, and it must never take anything that did not
come from the maintainer. This file is the contract between the release
tooling, which exists, and the updater in the firmware, which does not yet.

**Status.** In place: the two-slot flash layout, the version stamp, the size
guard, the signing tools, the web installer and the Pages workflow. Not in
place: the updater. Until it lands, `include/ota_public_key.h` is a placeholder
(`config::kOtaPublicKeyIsPlaceholder == true`) and every update must be
refused.

Related: [RELEASING.md](RELEASING.md) (how the maintainer publishes),
`scripts/ota_common.py` (the constants below, as code),
`tools/ota/test_release_tools.py` (signs with a throwaway key and verifies the
way a radar must).

---

## Flash layout

`partitions/plane_radar.csv`, 4 MB:

| Name | Type | Offset | Size | What for |
|------|------|--------|------|----------|
| `nvs` | data/nvs | `0x9000` | `0x5000` | Wi-Fi, location, settings. **Same offset and size as every earlier layout**, so they survive a re-flash. |
| `otadata` | data/ota | `0xe000` | `0x2000` | Which app slot boots, and its trial state. |
| `app0` | app/ota_0 | `0x10000` | `0x1C0000` | App slot A (1.75 MB). |
| `app1` | app/ota_1 | `0x1D0000` | `0x1C0000` | App slot B. |
| `logos` | data/`0x40` | `0x390000` | `0x60000` | The private airline logo pack (384 KB), flashed once over USB. Updates never touch it. |
| `coredump` | data/coredump | `0x3F0000` | `0x10000` | Unchanged. |

- An update is written to whichever slot is **not** running, so every image
  must fit **both**. `scripts/size_guard.py` fails the build when
  `firmware.bin` exceeds `0x1C0000` and warns above 90 %. No image contains
  airline logos (they are in the `logos` partition); an image is ~1.49 MB (81 %).
- The bootloader (ESP-IDF 4.4 in Arduino-ESP32 2.0.14) will not start an image
  longer than its partition. The size guard is a hard limit, not a hint.
- `logos` is a raw partition: nothing mounts it, and the firmware only reads
  it. The owner flashes the private logo pack into it once over USB:
  `python3 scripts/build_airline_brands.py --logos` writes
  `.cache/logo-pack.bin`, then
  `esptool.py --chip esp32c3 write_flash 0x390000 .cache/logo-pack.bin`.
  `src/data/airline_logos.cpp` finds the partition with
  `esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "logos")`
  and checks the pack's header and CRC; without a valid pack the card shows
  airline codes. An update rewrites an app slot only, so it never removes the
  logos. Erasing the whole flash does; flash the pack again then. On a radar
  that had an older layout the partition still holds whatever was at
  `0x390000` before, which fails that check and is not used.
- **An update over the air cannot change this table.** The first move to it
  happens over USB: the web installer (leave *Erase* unticked to keep the
  settings) or `pio run -t upload`. Writing `firmware-merged.bin` at `0x0`
  also works but fills the `nvs` range with `0xFF`, so it wipes the settings.
- A radar still on the old single 3 MB slot layout has no second slot:
  `esp_ota_get_next_update_partition()` returns `NULL` there, and the updater
  must treat that as "updates not possible", not as an error loop.

## Versions

`scripts/version.py` sets `PLANE_RADAR_VERSION` from
`git describe --tags --dirty --always --match "v[2-9].*"`:
`v2.1.0` at a tag, `v2.1.0-3-g1a2b3c4` after it, `-dirty` with local changes,
a bare hash without a tag, `dev` without git.

`util::release_version` (host-tested in `test/test_ota`) is the rule:

- A **release** is exactly `vMAJOR.MINOR.PATCH`, decimal, no leading zeros,
  each part 0..65535. `make_release.py` publishes nothing else.
- A radar installs an offer only if **both** its own version and the offered
  one are releases and the offer is **strictly newer**
  (`util::release_version::shouldInstall`). No downgrades, no reinstalls, and
  a development build is never replaced behind its owner's back.
- The offered version is also part of the firmware URL. Because only digits
  and dots get through, a manifest cannot point the download anywhere else.

## Where the files are

GitHub Pages serves `site/` (`.github/workflows/pages.yml`):

| URL | Content |
|-----|---------|
| `https://ericcheng0904-pixel.github.io/plane-radar/ota/manifest.json` | what radars poll |
| `https://ericcheng0904-pixel.github.io/plane-radar/ota/firmware-<version>.bin` | the signed image |
| `https://ericcheng0904-pixel.github.io/plane-radar/` | web installer (`index.html`, `manifest.json`, `flash/*.bin`) |

The firmware URL is built on the device from the manifest's `version`; it is
not a field. Only the current image is kept on the site.

`*.github.io` presents a Let's Encrypt chain (`YR1` -> `ISRG Root YR`,
cross-signed by `ISRG Root X1`). X1 is in the bundle the firmware embeds
(`data/cert/x509_crt_bundle.bin`), so the chain verifies today.

## The manifest

`site/ota/manifest.json`, written by `scripts/make_release.py`, at most
1024 bytes:

```json
{
  "version": "v2.1.0",
  "size": 1493856,
  "sha256": "b868…293e",
  "sig": "MEUCIEqb…arw=",
  "min_free_heap_hint": 57344,
  "notes": "Larger labels"
}
```

| Field | Type | Meaning |
|-------|------|---------|
| `version` | string | A release (see above). |
| `size` | integer | Exact byte length of the image; `1 ..= 0x1C0000`. |
| `sha256` | string | 64 lowercase hex digits: SHA-256 of the image. |
| `sig` | string | Base64 of an ASN.1 DER ECDSA signature (≤ 72 bytes) over SHA-256 of the image. |
| `min_free_heap_hint` | integer | Free heap (bytes) to have before starting: a TLS handshake (48 KB in `services::tls` when its record buffers are not reserved) plus `Update`'s 4 KB sector buffer and margin. The device uses the larger of this and its own minimum. |
| `notes` | string | One line, ≤ 200 characters, for the settings page. Display only. |

Unknown fields are ignored. Anything missing or of the wrong type means "no
update".

## Signature and key

- **Algorithm:** ECDSA over NIST P-256 (secp256r1) with SHA-256, signing the
  exact bytes of `firmware-<version>.bin`.
- **Public key:** `include/ota_public_key.h`, generated by
  `scripts/ota_keygen.py`: `config::kOtaPublicKey[65]` is the uncompressed SEC1
  point `0x04 ‖ X ‖ Y` (32 bytes each, big-endian),
  `config::kOtaPublicKeyIsPlaceholder` says whether it is real, and
  `config::kOtaPublicKeyFingerprint` is the first 8 bytes of its SHA-256 in hex
  (for logs; `make_release.py` prints the same).
- **Private key:** the encrypted Actions secret `OTA_SIGNING_KEY`, used only by
  the Release workflow, which signs each tagged release; never in the
  repository ([RELEASING.md](RELEASING.md)). Whoever can push a tag can
  publish an update: keep two-factor sign-in on the owning account.

With the mbedTLS 2.28 that Arduino-ESP32 2.0.14 ships:

```cpp
#include <mbedtls/base64.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/sha256.h>
#include "ota_public_key.h"

// digest: the SHA-256 finished after the last byte was written (below).
bool signatureValid(const uint8_t digest[32], const char* sig_b64) {
  if (config::kOtaPublicKeyIsPlaceholder) return false;
  uint8_t sig[MBEDTLS_ECDSA_MAX_LEN];
  size_t sig_len = 0;
  if (mbedtls_base64_decode(sig, sizeof(sig), &sig_len,
                            reinterpret_cast<const uint8_t*>(sig_b64), strlen(sig_b64)) != 0) {
    return false;
  }
  mbedtls_ecdsa_context ecdsa;
  mbedtls_ecdsa_init(&ecdsa);
  bool ok = mbedtls_ecp_group_load(&ecdsa.grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_ecp_point_read_binary(&ecdsa.grp, &ecdsa.Q, config::kOtaPublicKey,
                                          sizeof(config::kOtaPublicKey)) == 0 &&
            mbedtls_ecp_check_pubkey(&ecdsa.grp, &ecdsa.Q) == 0 &&
            mbedtls_ecdsa_read_signature(&ecdsa, digest, 32, sig, sig_len) == 0;
  mbedtls_ecdsa_free(&ecdsa);
  return ok;
}
```

`MBEDTLS_ECDSA_C` and `MBEDTLS_ECP_DP_SECP256R1_ENABLED` are on in the
framework's `sdkconfig.h`. The all-zero placeholder fails
`mbedtls_ecp_point_read_binary` even if the flag were ignored.

## The update, step by step

Everything below runs on the **fetch task** (`adsbFetchTask` in
`src/main.cpp`), in place of an ADS-B fetch, never on the main loop, and with
**no other TLS connection open**: a handshake needs ~45 KB (33 KB of it the
record buffers reserved at boot) and the C3 has room for one.

**When.** 2–5 minutes after every boot (random, so a fleet does not arrive
at once), then once a day; the daily restart makes that once a day anyway.
Checking at every boot means a release that is confirmed but then crashes
every so often still reaches its fix. Never while the running image is still
on trial: Arduino's `Update` does not stop that, so `services::ota` does.
"Check for an update now" on the settings page checks on the next pass.

**Certificates.** Pages is reached like the feeds (`services::tls`, host
`kPages`): checked against the embedded bundle, and after six handshakes in a
row rejected for the certificate, unchecked. Updates must outlive a change of
certificate chain on GitHub's side, and the signature, the version marker and
"strictly newer" already make an unchecked connection harmless.

1. **Preconditions.** `kOtaPublicKeyIsPlaceholder` is false;
   `config::kFirmwareVersion` is a release; `esp_ota_get_next_update_partition(NULL)`
   is not `NULL`; Wi-Fi is up; free heap ≥ the larger of the last manifest's
   `min_free_heap_hint` and 56 KB, and `services::tls::enoughMemory()`.
2. **Fetch the manifest** over HTTPS with `config::kUserAgent`, a 12 s
   handshake and 10 s read timeout, and the `Accept-Encoding: identity` that
   `HTTPClient` sends by default (never ask for gzip: `size` and the hash are
   of the uncompressed bytes). Require HTTP 200 and a body of at most 1024
   bytes; parse it into a fixed `JsonDocument`. Close the connection.
3. **Decide.** `version` passes `shouldInstall(kFirmwareVersion, version)`;
   `size` is `1 ..= 0x1C0000` **and** `<= next_partition->size`; `sha256` is 64
   hex digits; `sig` decodes to 8..72 bytes. If this version and `sha256`
   failed their trial within the last day, or three times, skip it (see
   below).
   Otherwise stop here: no update.
4. **Open the image** `ota/firmware-<version>.bin` on the same host. Require
   HTTP 200 and `Content-Length == size`. No redirects to other hosts.
5. **`Update.begin(size, U_FLASH)`.** It picks the inactive slot and keeps the
   image unbootable until `end()`: the first 16 bytes are held back and written
   last.
6. **Stream.** Read into a fixed buffer (1–4 KB, static or on the task stack;
   no per-chunk allocation). For every chunk: `mbedtls_sha256_update_ret`, then
   `Update.write`. Also look for the **version marker**
   `config::kUserAgent` as built for the offered version, i.e.
   `"PlaneRadar/<version> (+https://github.com/ericcheng0904-pixel/plane-radar)"`,
   carrying the last `len - 1` bytes over between chunks. Abort on a read
   timeout, a short read, more bytes than `size`, or any `Update` error.
7. **Close the connection** before verifying, to give its memory back.
8. **Verify, in this order:** exactly `size` bytes written and
   `Update.isFinished()`; the SHA-256 equals `sha256` (a mismatch means a
   corrupted download; retry later); `signatureValid(digest, sig)`; the marker
   was seen. Any failure: `Update.abort()` and stop. The slot is left without
   its first bytes, so it can never boot.
9. **Only then `Update.end(true)`.** It writes the held-back bytes and calls
   `esp_ota_set_boot_partition()`. (`true` would also accept a short image,
   which is why step 8 checks the length first; `Update.end()` without it is
   equivalent once `isFinished()` is true.)
10. **Remember the attempt** in NVS: offered version, its `sha256`, the
    running version. Then `ESP.restart()`.

Saved Wi-Fi, location and settings are in `nvs`, which an update never
touches.

## Trial boot and rollback

The framework's bootloader is built with
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. After `esp_ota_set_boot_partition()`
the new image boots once as `ESP_OTA_IMG_PENDING_VERIFY`; if it restarts before
it is marked valid, for **any** reason (crash, watchdog, brown-out, a
`device_health` restart, power cut), the bootloader marks it aborted and starts
the previous image again.

Arduino-ESP32 marks every image valid in `initArduino()`, before `setup()`,
unless the sketch overrides a weak hook. The updater **must** define

```cpp
extern "C" bool verifyRollbackLater() { return true; }
```

and then, on boot, if `esp_ota_get_state_partition(esp_ota_get_running_partition(), &s)`
gives `ESP_OTA_IMG_PENDING_VERIFY`, run on trial:

- call `esp_ota_mark_app_valid_cancel_rollback()` only once **Wi-Fi has
  connected, the display has drawn a radar frame, one ADS-B fetch has
  succeeded, and the image has run 15 minutes** (one that crashes after a
  few minutes must not be confirmed);
- if that has not happened 20 minutes after Wi-Fi first came up, or 40
  minutes after boot in any case (a timer: an image that cannot join Wi-Fi
  sits in the setup hotspot, where `loop()` never runs), call
  `esp_ota_mark_app_invalid_rollback_and_reboot()`, and `esp_restart()` if
  that returns.

On every boot of a confirmed image, check `esp_ota_get_last_invalid_partition()`
together with the attempt recorded in step 10: if the version that was
attempted is not the one running, that version **failed its trial**. Count
failures per version + `sha256` in NVS, wait at least a day before the next
try, and stop trying that one after three; a re-published fix has a newer
version or a different `sha256` and starts from zero. One failure is not
enough to give up: someone unplugging the radar during its trial also rolls it
back. Without a limit, a radar that can never pass the trial would download and
reject the same image every day for years.

Images flashed over USB are `ESP_OTA_IMG_UNDEFINED` (the installer writes
`boot_app0.bin`, which selects `app0` with no state), are treated as confirmed,
and never roll back.

## What protects what

| Threat | Stopped by |
|--------|------------|
| Someone on the network (or the Wi-Fi) serves their own firmware | The signature: they do not have the private key. |
| A download corrupted in transit | SHA-256 and the signature, before the image can boot. |
| A truncated or interrupted download | Exact `size`, `isFinished()`, and `Update` keeping the image unbootable until `end()`. |
| An image that does not run | Trial boot and automatic rollback. |
| A manifest that relabels an **old** signed image as new | The version marker: the signed image itself must say the version the manifest claims. The manifest is not signed; this binds it to what is. |
| A downgrade to an older, signed but buggy release | `shouldInstall`: strictly newer only. |
| A manifest that points the download elsewhere | The URL is built from a version that can only contain digits and dots. |
| A radar someone flashed by hand being overwritten | Development versions never update. |
| Publishing an image with the airline logos (trademarks) | `make_release.py` and the CI checks refuse it. |

**TLS.** The signature, not TLS, is what keeps foreign firmware out. The
updater should verify the certificate (bundle, as `services::tls` does) and
may fall back to not verifying after repeated certificate failures, like the
ADS-B feeds. That fallback is safe because of the rows above, and it matters:
if GitHub Pages ever moves to a root the embedded bundle lacks, an updater that
insisted on the certificate could never install the release that brings the
new root.

**Limits.** Whoever holds the private key can ship firmware to every radar.
Whoever controls the Pages site but not the key can only withhold updates or
re-offer a signed release that is newer than what a radar runs. Losing the key
means the next image has to go on over USB.

## Memory and time

- Steady free heap is ~45 KB with a largest block of ~12 KB, beside the 33 KB
  reserved at boot for mbedTLS's two record buffers (since v2.0.14; before,
  ~81 KB with a largest block of ~35–39 KB). The update gets the frame
  buffer's 57.6 KB on top. The TLS handshake takes the reserved buffers and
  12–15 KB more, `Update` 4 KB plus 16 bytes, SHA-256 a few hundred bytes, the
  signature check a few KB after the connection is closed. The heap hint and
  `services::tls::enoughMemory()` keep a fragmented heap from failing half
  way: skip and retry later.
- A ~1.5 MB download takes roughly 15–60 s. Flash erases (64 KB blocks) stall
  the single core's cache for tens to hundreds of milliseconds each, so the
  display stutters during the download, and the fetch task delivers no ADS-B
  data. `device_health` restarts a radar whose fetch task has not tried a
  flight fetch for 180 s, so the whole download is cut off at 150 s and
  retried the next boot. Reads go through `read(buf, n)`; `readBytes()`
  would go a byte at a time through TLS.
- A download that fails is not retried until the next boot. An image that
  fails its hash, signature or version check counts a strike against that
  version and image; after three it is not downloaded again.
- Aircraft go stale after `kAircraftStaleMs` (90 s), so a radar that is
  updating shows "No flight data" briefly. A message on screen during the
  download ("Updating…") is kinder.

## Checking an updater on the device

When the updater lands, before it ships:

- [ ] Placeholder key: no update is attempted, and the log says why.
- [ ] Publish a test release to a fork's Pages, signed with a throwaway key
      built into the test image: the radar finds it, downloads, restarts into
      it, and confirms it after Wi-Fi + frame + fetch (`otadata` state VALID).
- [ ] Same with an image that panics in `setup()`: it rolls back to the
      previous image by itself, and that version is not tried again for a day,
      and never after three failures.
- [ ] Same with Wi-Fi switched off after the restart: rollback after the
      10-minute trial, the old image works, settings intact.
- [ ] A manifest whose `sha256` or `sig` is wrong in one character: no reboot,
      the previous image keeps running, the log names the check that failed.
- [ ] A manifest with a lower or equal version, or a `-dirty` version: ignored.
- [ ] Pull the plug half way through a download: the radar boots the old
      image.
- [ ] Free heap during the download stays above ~15 KB; no `device_health`
      restart during it.
- [ ] Wi-Fi, location and settings are unchanged after an update.
