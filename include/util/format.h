#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Text formatting for the radar's labels. Free of Arduino headers so the host
 * test suite can cover it.
 */
namespace util::format {

/** "12,400": thousands separated, as altitudes are read out. Always terminates. */
void thousands(int32_t value, char* out, size_t out_len);

/**
 * Flight number as printed on a ticket: the airline's IATA code in place of the
 * three-letter ICAO prefix ("SWA1966" + "WN" -> "WN1966"). Without an IATA code,
 * or when the callsign is not longer than a bare prefix, the callsign is copied
 * as received.
 */
void flightNumber(const char* callsign, const char* iata, char* out,
                  size_t out_len);

/**
 * The leg of a multi-stop route that arrives at (`arriving`) or leaves
 * `airport`: "SNA-ORD-IAD" arriving ORD -> "SNA-ORD", leaving ORD ->
 * "ORD-IAD"; "ORD-CHS-ORD" arriving ORD -> "CHS-ORD". False when `airport` is
 * not a stop with such a leg (nothing written then).
 */
bool routeLeg(const char* codes, const char* airport, bool arriving, char* out,
              size_t out_len);

}  // namespace util::format
