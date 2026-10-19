#!/usr/bin/env python3
"""Generate airline brands (short name, IATA code, colour) for the info card.

For every ICAO code listed in data/airline_brand_codes.txt this writes the IATA
code, a short display name and a brand colour into the PUBLIC table
src/data/airline_brands_table.cpp (and its header). That table holds no image
data and is safe to publish; without logos the card shows the airline's code on
its brand colour.

Only with --logos does it also write 32x32 logos in panel-native RGB565 into a
PRIVATE logo pack, .cache/logo-pack.bin (git-ignored). The pack is flashed once,
over USB, into the radar's own "logos" flash partition:

    esptool.py --chip esp32c3 write_flash 0x390000 .cache/logo-pack.bin

No firmware image ever contains logos, so every image is publishable, and an
over-the-air update, which only rewrites an app slot, never removes them.

Sources
  * Names and IATA codes: OpenFlights airlines.dat (ODbL),
    https://openflights.org/data.html
  * Brand colours: curated in BRAND_COLOURS below (a colour is not a logo).
  * Logos (--logos only): FlightAware symbol logos as collected in
    https://github.com/Jxck-S/airline-logos (flightaware_logos/<ICAO>.png).
    These are the airlines' trademarks and the collection carries no licence:
    fine on your own device, but NEVER commit or publish the logo pack.
    scripts/check_no_logos.py fails CI if logo arrays reach a tracked file.

Logo colours
  The GC9A01 panel shows R and B swapped relative to what LovyanGFX writes
  (see panelColor() in radar_display.cpp), so pixels are stored pre-swapped.
  Logos have soft alpha edges; they are blended here onto CARD_BG, the card
  colour they are drawn on, and fully transparent pixels become LOGO_KEY for
  pushImage's transparent-colour test. Logos too dark to read on the card get
  a light rounded badge behind them.

Usage: python3 scripts/build_airline_brands.py            public table only
       python3 scripts/build_airline_brands.py --logos    + private logos (needs Pillow)
"""

from __future__ import annotations

import argparse
import csv
import io
import struct
import subprocess
import sys
import urllib.request
import zlib
from pathlib import Path
from typing import TYPE_CHECKING

# Pillow is imported only for --logos: the public table needs nothing beyond
# the standard library.
if TYPE_CHECKING:
    from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
CODES_FILE = ROOT / "data" / "airline_brand_codes.txt"
CACHE = ROOT / ".cache" / "airline_logos"
TABLE_FILE = ROOT / "src" / "data" / "airline_brands_table.cpp"
HEADER_FILE = ROOT / "include" / "data" / "airline_brands.h"
# Must stay in step with .gitignore and the __has_include in airline_logos.cpp.
LOGO_PACK = ROOT / ".cache" / "logo-pack.bin"
# Pack format, read by src/data/airline_logos.cpp (little-endian):
#   "PRLG", u16 version, u16 count, u16 size, u16 transparent key, u32 CRC-32
#   of everything after the header; count x (char icao[4], u32 offset), sorted
#   by ICAO code; then each logo, size*size u16 pixels as the card draws them.
PACK_MAGIC = b"PRLG"
PACK_VERSION = 1
# The "logos" partition in partitions/plane_radar.csv.
PACK_MAX_BYTES = 0x60000
OPENFLIGHTS_URL = (
    "https://raw.githubusercontent.com/jpatokal/openflights/master/data/airlines.dat"
)
LOGO_URL = (
    "https://raw.githubusercontent.com/Jxck-S/airline-logos/main/flightaware_logos/{}.png"
)

# Only the card's 32 px slot draws logos; the 16 px set was never drawn.
LOGO_SIZE = 32
MAX_NAME = 17
# Must match kCardBg in src/ui/info_card.cpp.
CARD_BG = (14, 18, 30)
BADGE_BG = (236, 238, 242)
# Luminance below which a logo is drawn on BADGE_BG instead of the dark card.
DARK_LOGO_LUMA = 70
# Transparent colour for pushImage; a real pixel that lands on it is nudged.
LOGO_KEY = 0xF81F

# Names as people say them. OpenFlights has legal names ("Delta Air Lines"),
# and the radar's own table cuts those at 16 characters.
SHORT_NAMES = {
    "AAL": "American", "UAL": "United", "DAL": "Delta", "SWA": "Southwest",
    "SKW": "SkyWest", "RPA": "Republic", "ENY": "Envoy", "EDV": "Endeavor",
    "ASA": "Alaska", "JBU": "JetBlue", "FFT": "Frontier", "NKS": "Spirit",
    "AAY": "Allegiant", "SCX": "Sun Country", "MXY": "Breeze", "JIA": "PSA Airlines",
    "ASH": "Mesa", "GJS": "GoJet", "UCA": "CommutAir", "PDT": "Piedmont",
    "QXE": "Horizon", "HAL": "Hawaiian", "FDX": "FedEx", "UPS": "UPS",
    "GTI": "Atlas Air", "ABX": "ABX Air", "ATN": "ATI", "ACA": "Air Canada",
    "JZA": "Jazz", "ROU": "Air Canada Rouge", "WJA": "WestJet", "POE": "Porter",
    "FLE": "Flair", "TSC": "Air Transat", "AMX": "Aeromexico", "VOI": "Volaris",
    "VIV": "Viva Aerobus", "CMP": "Copa", "AVA": "Avianca", "BAW": "British Airways",
    "VIR": "Virgin Atlantic", "EIN": "Aer Lingus", "DLH": "Lufthansa",
    "AFR": "Air France", "KLM": "KLM", "IBE": "Iberia", "SWR": "Swiss",
    "AUA": "Austrian", "LOT": "LOT", "SAS": "SAS", "FIN": "Finnair",
    "ICE": "Icelandair", "TAP": "TAP Portugal", "ITY": "ITA Airways",
    "THY": "Turkish", "UAE": "Emirates", "QTR": "Qatar Airways", "ETD": "Etihad",
    "ELY": "El Al", "AIC": "Air India", "ANA": "ANA", "JAL": "Japan Airlines",
    "KAL": "Korean Air", "AAR": "Asiana", "CPA": "Cathay Pacific", "EVA": "EVA Air",
    "CAL": "China Airlines", "CCA": "Air China", "CES": "China Eastern",
    "CSN": "China Southern", "SIA": "Singapore", "QFA": "Qantas",
    "ANZ": "Air New Zealand", "RYR": "Ryanair", "EZY": "easyJet", "WZZ": "Wizz Air",
    "VLG": "Vueling", "EWG": "Eurowings", "NAX": "Norwegian", "BEL": "Brussels",
    "CFG": "Condor", "TOM": "TUI", "EJA": "NetJets", "LXJ": "Flexjet",
    "CKS": "Kalitta", "CLX": "Cargolux", "CAO": "Air China Cargo",
    "BOX": "AeroLogic", "GEC": "Lufthansa Cargo", "MXA": "Mexicana",
    "SVA": "Saudia", "ETH": "Ethiopian", "RAM": "Royal Air Maroc",
    "MSR": "EgyptAir", "KQA": "Kenya Airways", "SAA": "South African",
    "TAM": "LATAM Brasil", "LAN": "LATAM", "GLO": "Gol", "AZU": "Azul",
    "ARG": "Aerolineas", "IGO": "IndiGo", "AXB": "Air India Express",
    "AIQ": "Thai AirAsia", "AXM": "AirAsia", "THA": "Thai", "MAS": "Malaysia",
    "GIA": "Garuda", "PAL": "Philippine", "CEB": "Cebu Pacific", "HVN": "Vietnam",
    "VJC": "VietJet", "JST": "Jetstar", "VOZ": "Virgin Australia",
    "CXA": "Xiamen Air", "CHH": "Hainan", "CSC": "Sichuan", "CSZ": "Shenzhen",
    "APJ": "Peach", "SKY": "Skymark", "JJA": "Jeju Air", "TWB": "T'way",
    "FDB": "flydubai", "ABY": "Air Arabia", "OMA": "Oman Air", "GFA": "Gulf Air",
    "KAC": "Kuwait Airways", "RJA": "Royal Jordanian", "MEA": "Middle East",
    "PGT": "Pegasus", "AEE": "Aegean", "CTN": "Croatia", "TVS": "Smartwings",
    "BTI": "airBaltic", "SXS": "SunExpress", "DAH": "Air Algerie",
}


# IATA codes OpenFlights is missing (mostly regionals and newer airlines).
IATA_OVERRIDES = {
    "EDV": "9E", "ENY": "MQ", "JIA": "OH", "MXY": "MX", "FLE": "F8", "ITY": "AZ",
    "BEL": "SN", "GFA": "GF", "ROU": "RV", "VIV": "VB", "VJC": "VJ", "APJ": "MM",
    "JJP": "GK", "JSX": "XE", "MSC": "SM", "BOX": "3S", "HBH": "NS", "QLK": "QF",
    "RUK": "RK", "WUK": "W9", "CAO": "CA",
}
SHORT_NAMES.update({
    "FRG": "Freight Runners", "RUK": "Ryanair UK", "WUK": "Wizz Air UK",
    "QLK": "QantasLink", "JJP": "Jetstar Japan", "HBH": "Hebei Airlines",
})

# Badge colour per airline, 0xRRGGBB as it should look on the glass. Taken from
# the dominant colour of each logo, then corrected by hand where that misleads
# (Southwest's heart is mostly red, but the airline reads as blue; Spirit's logo
# is grey, the aircraft are yellow). Airlines missing here, mostly ones whose
# logo is black or grey, get the card's neutral badge. The card picks dark or
# white text per colour, so any colour is legible.
BRAND_COLOURS = {
    "AAL": 0x0078D2, "AAR": 0xF6D852, "AAY": 0xF79718, "ABX": 0xF92729,
    "ABY": 0xFE0041, "ACA": 0xF51428, "ADO": 0xFFF34A, "AEE": 0x012475,
    "AFR": 0x002157, "AIC": 0xDC0C28, "AIQ": 0xEF0101, "AMF": 0x04538C,
    "AMX": 0x020343, "ANA": 0x082F8D, "APJ": 0xD45C9C, "ARG": 0x0084CE,
    "ASA": 0x09436A, "ATN": 0x0B4788, "AUA": 0xD51C00, "AVA": 0xFD0101,
    "AXB": 0xFF6508, "AXM": 0xDD1C20, "AZU": 0x4ABCD6, "BAW": 0x075AAA,
    "BCS": 0xFFCC00, "BEL": 0xEE0029, "BOX": 0xAE9020, "BTI": 0xCEDA31,
    "CAL": 0xECADB6, "CAO": 0xFA2020, "CAP": 0x8FB3D4, "CCA": 0xFA2020,
    "CDG": 0x103C6A, "CEB": 0x00B4F3, "CES": 0xED0211, "CHH": 0xF10010,
    "CLX": 0xD40334, "CMP": 0x0C3A63, "CPA": 0x046469, "CQH": 0x088455,
    "CSC": 0xED010C, "CSN": 0x0093D3, "CSZ": 0xD60820, "CTN": 0xC60113,
    "CTV": 0x087136, "CUA": 0xD6006B, "CXA": 0x008DCB, "DAH": 0xF12018,
    "DAL": 0xC01933, "DLH": 0x052966, "EDV": 0x701824, "EIN": 0x84CA41,
    "ELY": 0x183299, "ENY": 0x2A1467, "ETD": 0x86582E, "ETH": 0xF5D410,
    "EVA": 0x009A42, "EWG": 0x90144C, "EXS": 0xEB2218, "EZY": 0xFF6500,
    "FDA": 0xFCF300, "FDB": 0x24568B, "FDX": 0x4B1390, "FFM": 0xFFCE00,
    "FFT": 0x006942, "FIN": 0x091563, "FRG": 0x0232C6, "GEC": 0x011650,
    "GFA": 0x887A56, "GIA": 0x2B98A9, "GJS": 0x034774, "GLO": 0xFC7121,
    "GTI": 0x022664, "HAL": 0x463A90, "HBH": 0xE93829, "HVN": 0x006885,
    "IBE": 0xFCAB00, "ICE": 0x001B74, "IGO": 0x25316D, "ITY": 0x1F4CA0,
    "JAL": 0xD30303, "JBU": 0x0033A0, "JIA": 0x0078D2, "JJA": 0xF75920,
    "JJP": 0xFE5710, "JST": 0xFE5710, "JSX": 0xF43934, "JZA": 0xF51428,
    "KAC": 0x182A5A, "KAL": 0x01186B, "KLM": 0x00ACED, "KQA": 0xF31B20,
    "LAN": 0x16018E, "LOT": 0x182F62, "MAS": 0x002A5A, "MEA": 0x007257,
    "MSC": 0xDDAD67, "MSR": 0x042B64, "MXA": 0x182541, "MXD": 0xCB2558,
    "MXY": 0xB6CBF8, "NKS": 0xFFE600, "OMA": 0xB59658, "PAL": 0xFE0000,
    "PDT": 0x00518C, "PGT": 0xFFA900, "POE": 0x002542, "QFA": 0xF20101,
    "QLK": 0xF20101, "QTR": 0x53042E, "QXE": 0x01426A, "RAM": 0xC1272D,
    "RAX": 0x4F452B, "RJA": 0xB3894B, "ROU": 0xB02C4E, "RUK": 0x073590,
    "RXA": 0x002F6A, "RYR": 0x073590, "SAA": 0xFCB310, "SAS": 0x1205A6,
    "SCX": 0xFB8630, "SIA": 0xFEAA00, "SJO": 0x008A55, "SKW": 0x055AA5,
    "SKY": 0x16368D, "SLI": 0x020343, "SVA": 0x016C32, "SWA": 0x304CB2,
    "SWR": 0xE90202, "SXS": 0x004590, "TAM": 0x29038F, "TAP": 0xEE2B2A,
    "THA": 0x5E2A84, "THY": 0xD02225, "TOM": 0x70CBF4, "TRA": 0x07A759,
    "TSC": 0x37B9EE, "TVS": 0xFC9000, "TWB": 0xD43031, "UAE": 0xB01C20,
    "UAL": 0x005DAA, "UCA": 0x184485, "UPS": 0x644117, "VIR": 0xAD1A27,
    "VIV": 0x75CC00, "VJC": 0xCF2129, "VLG": 0xFFCD00, "VOI": 0x942593,
    "VOZ": 0xEB0408, "VTE": 0x026EB2, "VTM": 0xF08C53, "WJA": 0x00ADA9,
    "WUK": 0x4F194A, "WZZ": 0x8F216B,
}

LOGO_WARNING = """\
WARNING: {path} holds airline logos.
  They are the airlines' trademarks (FlightAware's, via Jxck-S/airline-logos,
  no licence). Flash the pack onto your own radar and keep it to yourself:
  never commit it, publish it, or attach it to a release. .gitignore keeps
  .cache/ out of git and scripts/check_no_logos.py fails CI if logo data gets
  in anyway."""


def fetch_openflights() -> dict[str, tuple[str, str]]:
    cache = CACHE.parent / "airlines.dat"
    if not cache.exists():
        cache.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(OPENFLIGHTS_URL, timeout=60) as resp:
            cache.write_bytes(resp.read())
    rows: dict[str, tuple[str, str]] = {}
    for row in csv.reader(io.StringIO(cache.read_text(encoding="utf-8"))):
        if len(row) < 8:
            continue
        name, iata, icao, active = row[1], row[3], row[4], row[7]
        if len(icao) != 3 or not icao.isalpha():
            continue
        iata = iata if len(iata) == 2 and iata.isalnum() else ""
        # Prefer active rows: codes get reused after an airline folds.
        if icao not in rows or active == "Y":
            rows[icao] = (name, iata)
    return rows


def short_name(icao: str, legal: str | None) -> str:
    if icao in SHORT_NAMES:
        return SHORT_NAMES[icao]
    name = (legal or icao).strip()
    for suffix in (" Airlines", " Air Lines", " Airways", " Aviation", " Inc.", " Ltd"):
        if name.endswith(suffix) and len(name) > MAX_NAME:
            name = name[: -len(suffix)]
    return name[:MAX_NAME].rstrip()


def fetch_logo(icao: str) -> Image.Image | None:
    from PIL import Image

    path = CACHE / f"{icao}.png"
    if not path.exists():
        CACHE.mkdir(parents=True, exist_ok=True)
        try:
            with urllib.request.urlopen(LOGO_URL.format(icao), timeout=30) as resp:
                path.write_bytes(resp.read())
        except Exception as exc:  # noqa: BLE001 - a missing logo is not fatal
            print(f"  {icao}: no logo ({exc})")
            return None
    return Image.open(path).convert("RGBA")


def luma(rgb: tuple[int, int, int]) -> float:
    r, g, b = rgb
    return 0.299 * r + 0.587 * g + 0.114 * b


def panel565(r: int, g: int, b: int) -> int:
    # Panel shows R/B swapped: store (b, g, r) so the glass shows (r, g, b).
    return ((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3)


def swap16(v: int) -> int:
    # LovyanGFX reads plain uint16_t image data as big-endian (swap565_t).
    return ((v & 0xFF) << 8) | (v >> 8)


def render(logo: Image.Image, size: int) -> list[int]:
    from PIL import Image, ImageDraw

    bbox = logo.getchannel("A").getbbox()
    if bbox:
        logo = logo.crop(bbox)
    opaque = [px[:3] for px in logo.getdata() if px[3] > 128]
    mean = tuple(sum(c) / len(opaque) for c in zip(*opaque)) if opaque else (255, 255, 255)
    badge = luma(mean) < DARK_LOGO_LUMA

    inner = size - (6 if badge else 2)
    w, h = logo.size
    scale = inner / max(w, h)
    logo = logo.resize((max(1, round(w * scale)), max(1, round(h * scale))), Image.LANCZOS)

    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    if badge:
        ImageDraw.Draw(canvas).rounded_rectangle(
            (0, 0, size - 1, size - 1), radius=size // 5, fill=BADGE_BG + (255,))
    canvas.alpha_composite(logo, ((size - logo.width) // 2, (size - logo.height) // 2))

    out = []
    for r, g, b, a in canvas.getdata():
        if a < 40:
            out.append(swap16(LOGO_KEY))
            continue
        t = a / 255.0
        r = round(r * t + CARD_BG[0] * (1 - t))
        g = round(g * t + CARD_BG[1] * (1 - t))
        b = round(b * t + CARD_BG[2] * (1 - t))
        v = panel565(r, g, b)
        out.append(swap16(v ^ 0x0001 if v == LOGO_KEY else v))
    return out


def read_codes() -> list[str]:
    codes = []
    for line in CODES_FILE.read_text().splitlines():
        code = line.split("#", 1)[0].strip().upper()
        if code:
            assert len(code) == 3 and code.isalpha(), code
            codes.append(code)
    return sorted(set(codes))


def table_source(brands: list[tuple[str, str, str, int | None]]) -> str:
    """The public table: names, codes and colours, never pixel data."""
    lines = [
        "// Generated by scripts/build_airline_brands.py -- do not edit.",
        "// Public: no logo data. Logos, when the radar has a logo pack, come from",
        "// its own flash partition, not from this repository (see airline_logos.cpp).",
        "",
        '#include "data/airline_brands.h"',
        "",
        "namespace data::airline_brands {",
        "",
        "const AirlineBrand kBrands[kBrandCount] = {",
    ]
    for icao, iata, name, colour in brands:
        assert '"' not in name and "\\" not in name, name
        assert len(iata) in (0, 2), (icao, iata)
        assert colour is None or 0 <= colour <= 0xFFFFFF, (icao, colour)
        c = "kNoColor" if colour is None else f"0x{colour:06X}"
        lines.append(f'    {{"{icao}", "{iata}", "{name}", {c}}},')
    lines += ["};", "", "}  // namespace data::airline_brands", ""]
    return "\n".join(lines)


def header_source(count: int) -> str:
    return f"""#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Airline short names, IATA codes and brand colours for the info card, sorted
 * by ICAO code. Generated by scripts/build_airline_brands.py -- see there for
 * sources.
 *
 * No logos here: they are the airlines' trademarks, so they live in an
 * optional private file behind data/airline_logos.h, and this table -- like
 * any firmware built from a clean checkout -- can be published.
 */
namespace data::airline_brands {{

/** AirlineBrand::color when no brand colour is known: the card's neutral badge. */
constexpr uint32_t kNoColor = 0x1000000;

struct AirlineBrand {{
  char icao[4];
  char iata[3];
  const char* name;
  /** Brand colour as it should look on the glass, 0xRRGGBB, or kNoColor. */
  uint32_t color;
}};

constexpr size_t kBrandCount = {count};
constexpr size_t kMaxNameLen = {MAX_NAME};
extern const AirlineBrand kBrands[kBrandCount];

/** Brand for an airline-shaped callsign ("UAL1531"), or nullptr. */
const AirlineBrand* brandForCallsign(const char* callsign);

}}  // namespace data::airline_brands
"""


def pack_bytes(logos: dict[str, list[int]]) -> bytes:
    items = sorted(logos.items())
    first = 16 + 8 * len(items)
    per_logo = LOGO_SIZE * LOGO_SIZE * 2
    index, pixels = bytearray(), bytearray()
    for n, (icao, px) in enumerate(items):
        index += icao.encode("ascii").ljust(4, b"\0")[:4]
        index += struct.pack("<I", first + n * per_logo)
        pixels += struct.pack(f"<{len(px)}H", *px)
    body = bytes(index + pixels)
    header = PACK_MAGIC + struct.pack("<HHHHI", PACK_VERSION, len(items), LOGO_SIZE,
                                      swap16(LOGO_KEY), zlib.crc32(body))
    return header + body


def write_logos(codes: list[str]) -> None:
    try:
        import PIL  # noqa: F401
    except ImportError:
        sys.exit("--logos needs Pillow: pip install Pillow")
    logos = {}
    for icao in codes:
        logo = fetch_logo(icao)
        if logo is not None:
            logos[icao] = render(logo, LOGO_SIZE)
    if not logos:
        print("no logos fetched; nothing written")
        return
    pack = pack_bytes(logos)
    if len(pack) > PACK_MAX_BYTES:
        sys.exit(f"logo pack is {len(pack):,} bytes; the partition holds {PACK_MAX_BYTES:,}")
    LOGO_PACK.parent.mkdir(parents=True, exist_ok=True)
    LOGO_PACK.write_bytes(pack)
    rel = LOGO_PACK.relative_to(ROOT)
    print(f"wrote {len(logos)} logos ({len(pack):,} bytes) to {rel}")
    print(f"flash it: esptool.py --chip esp32c3 write_flash 0x390000 {rel}")
    print(LOGO_WARNING.format(path=rel), file=sys.stderr)
    # 0: ignored. 1: not ignored, or tracked. Anything else: not a git checkout.
    try:
        ignored = subprocess.run(["git", "check-ignore", "-q", str(rel)], cwd=ROOT,
                                 capture_output=True, check=False).returncode
    except OSError:
        return
    if ignored == 1:
        print(f"ERROR: git does not ignore {rel} (or it is tracked). Fix .gitignore "
              "before committing anything.", file=sys.stderr)
        sys.exit(1)


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Generate the airline brand table (and, with --logos, the "
                    "private trademarked logo file).")
    ap.add_argument("--logos", action="store_true",
                    help=f"also write {LOGO_PACK.relative_to(ROOT)}: airline logos, "
                         "trademarked, for your own device only (needs Pillow)")
    args = ap.parse_args()

    names = fetch_openflights()
    brands = []
    for icao in read_codes():
        legal, iata = names.get(icao, (None, ""))
        iata = IATA_OVERRIDES.get(icao, iata)
        brands.append((icao, iata, short_name(icao, legal), BRAND_COLOURS.get(icao)))

    TABLE_FILE.write_text(table_source(brands))
    HEADER_FILE.write_text(header_source(len(brands)))
    neutral = [b[0] for b in brands if b[3] is None]
    print(f"wrote {len(brands)} brands ({len(brands) - len(neutral)} with a brand colour)")
    if neutral:
        print("  neutral badge: " + " ".join(neutral))
    unused = sorted(set(BRAND_COLOURS) - {b[0] for b in brands})
    if unused:
        print("  colours for codes not in the list: " + " ".join(unused))

    if args.logos:
        write_logos([b[0] for b in brands])


if __name__ == "__main__":
    main()
