#!/usr/bin/env python3
"""Generate the ICAO type designator -> display name table for the info card.

The feed only sends the ICAO designator ("B38M"); the card spells it out
("Boeing 737 MAX 8"). The list is curated by hand rather than pulled from a
database: it only needs the types that actually fly past, and every name has
to fit the card (MAX_NAME characters).

Usage: python3 scripts/build_aircraft_types.py
Writes src/data/aircraft_types_data.cpp and include/data/aircraft_types.h.
"""

from pathlib import Path

MAX_NAME = 20

TYPES = {
    # Airbus
    "A306": "Airbus A300-600", "A310": "Airbus A310", "A318": "Airbus A318",
    "A319": "Airbus A319", "A320": "Airbus A320", "A321": "Airbus A321",
    "A19N": "Airbus A319neo", "A20N": "Airbus A320neo", "A21N": "Airbus A321neo",
    "A332": "Airbus A330-200", "A333": "Airbus A330-300", "A338": "Airbus A330-800",
    "A339": "Airbus A330-900", "A343": "Airbus A340-300", "A346": "Airbus A340-600",
    "A359": "Airbus A350-900", "A35K": "Airbus A350-1000", "A388": "Airbus A380",
    "BCS1": "Airbus A220-100", "BCS3": "Airbus A220-300", "A3ST": "Airbus Beluga",
    "A400": "Airbus A400M",
    # Boeing
    "B712": "Boeing 717", "B722": "Boeing 727", "B732": "Boeing 737-200",
    "B733": "Boeing 737-300", "B734": "Boeing 737-400", "B735": "Boeing 737-500",
    "B736": "Boeing 737-600", "B737": "Boeing 737-700", "B738": "Boeing 737-800",
    "B739": "Boeing 737-900", "B37M": "Boeing 737 MAX 7", "B38M": "Boeing 737 MAX 8",
    "B39M": "Boeing 737 MAX 9", "B3XM": "Boeing 737 MAX 10", "B744": "Boeing 747-400",
    "B748": "Boeing 747-8", "B74S": "Boeing 747SP", "B752": "Boeing 757-200",
    "B753": "Boeing 757-300", "B762": "Boeing 767-200", "B763": "Boeing 767-300",
    "B764": "Boeing 767-400", "B772": "Boeing 777-200", "B77L": "Boeing 777-200LR",
    "B773": "Boeing 777-300", "B77W": "Boeing 777-300ER", "B778": "Boeing 777-8",
    "B779": "Boeing 777-9", "B788": "Boeing 787-8", "B789": "Boeing 787-9",
    "B78X": "Boeing 787-10", "K35R": "Boeing KC-135", "KC46": "Boeing KC-46",
    "C17": "Boeing C-17", "P8": "Boeing P-8 Poseidon", "E3TF": "Boeing E-3 Sentry",
    # McDonnell Douglas
    "DC10": "Douglas DC-10", "MD11": "McDonnell MD-11", "MD82": "McDonnell MD-82",
    "MD83": "McDonnell MD-83", "MD88": "McDonnell MD-88", "MD90": "McDonnell MD-90",
    # Embraer
    "E135": "Embraer ERJ-135", "E145": "Embraer ERJ-145", "E170": "Embraer E170",
    "E75L": "Embraer E175", "E75S": "Embraer E175", "E190": "Embraer E190",
    "E195": "Embraer E195", "E290": "Embraer E190-E2", "E295": "Embraer E195-E2",
    "E35L": "Embraer Legacy 600", "E545": "Embraer Legacy 450", "E550": "Embraer Praetor 600",
    "E50P": "Embraer Phenom 100", "E55P": "Embraer Phenom 300",
    # Bombardier / Canadair / de Havilland
    "CRJ1": "Bombardier CRJ100", "CRJ2": "Bombardier CRJ200", "CRJ7": "Bombardier CRJ700",
    "CRJ9": "Bombardier CRJ900", "CRJX": "Bombardier CRJ1000", "CL30": "Challenger 300",
    "CL35": "Challenger 350", "CL60": "Challenger 600", "GL5T": "Global 5000",
    "GLEX": "Global Express", "GL7T": "Global 7500", "LJ35": "Learjet 35",
    "LJ45": "Learjet 45", "LJ60": "Learjet 60", "LJ75": "Learjet 75",
    "DH8A": "Dash 8-100", "DH8B": "Dash 8-200", "DH8C": "Dash 8-300", "DH8D": "Dash 8-400",
    "DHC6": "DHC-6 Twin Otter", "DHC2": "DHC-2 Beaver",
    # ATR / Saab / others
    "AT43": "ATR 42-300", "AT45": "ATR 42-500", "AT46": "ATR 42-600", "AT72": "ATR 72",
    "AT75": "ATR 72-500", "AT76": "ATR 72-600", "SF34": "Saab 340", "SB20": "Saab 2000",
    "SU95": "Sukhoi Superjet", "C919": "COMAC C919", "AJ27": "COMAC ARJ21",
    "A124": "Antonov An-124", "AN12": "Antonov An-12", "AN26": "Antonov An-26",
    "IL76": "Ilyushin Il-76", "C130": "Lockheed C-130", "C30J": "Lockheed C-130J",
    "C5M": "Lockheed C-5M",
    # Cessna / Textron
    "C150": "Cessna 150", "C152": "Cessna 152", "C162": "Cessna Skycatcher",
    "C172": "Cessna 172", "C182": "Cessna 182", "C206": "Cessna 206",
    "C208": "Cessna Caravan", "C210": "Cessna 210", "C310": "Cessna 310",
    "C340": "Cessna 340", "C414": "Cessna 414", "C421": "Cessna 421",
    "C510": "Citation Mustang", "C525": "Citation CJ1", "C25A": "Citation CJ2",
    "C25B": "Citation CJ3", "C25C": "Citation CJ4", "C550": "Citation II",
    "C560": "Citation V", "C56X": "Citation Excel", "C680": "Citation Sovereign",
    "C68A": "Citation Latitude", "C700": "Citation Longitude", "C750": "Citation X",
    # Piper
    "P28A": "Piper Cherokee", "P28R": "Piper Arrow", "P32R": "Piper Saratoga",
    "PA18": "Piper Super Cub", "PA24": "Piper Comanche", "PA31": "Piper Navajo",
    "PA34": "Piper Seneca", "PA44": "Piper Seminole", "PA46": "Piper Malibu",
    "P46T": "Piper Meridian",
    # Beechcraft
    "BE20": "King Air 200", "BE9L": "King Air 90", "B350": "King Air 350",
    "BE35": "Beech Bonanza", "BE36": "Beech Bonanza", "BE55": "Beech Baron",
    "BE58": "Beech Baron", "BE40": "Beechjet 400",
    # Cirrus / Diamond / Pilatus / Daher / Honda
    "SR20": "Cirrus SR20", "SR22": "Cirrus SR22", "S22T": "Cirrus SR22T",
    "SF50": "Cirrus Vision Jet", "DA40": "Diamond DA40", "DA42": "Diamond DA42",
    "DA62": "Diamond DA62", "DV20": "Diamond DA20", "PC12": "Pilatus PC-12",
    "PC24": "Pilatus PC-24", "TBM7": "Daher TBM 700", "TBM8": "Daher TBM 850",
    "TBM9": "Daher TBM 900", "HDJT": "HondaJet",
    # Gulfstream / Dassault / Hawker
    "GLF4": "Gulfstream IV", "GLF5": "Gulfstream V", "GLF6": "Gulfstream G650",
    "G280": "Gulfstream G280", "GA5C": "Gulfstream G500", "GA6C": "Gulfstream G600",
    "G150": "Gulfstream G150", "GALX": "Gulfstream G200", "FA50": "Falcon 50",
    "F900": "Falcon 900", "F2TH": "Falcon 2000", "FA7X": "Falcon 7X", "FA8X": "Falcon 8X",
    "FA6X": "Falcon 6X", "H25B": "Hawker 800", "HA4T": "Hawker 4000",
    # Helicopters
    "R22": "Robinson R22", "R44": "Robinson R44", "R66": "Robinson R66",
    "EC30": "Airbus H130", "EC35": "Airbus H135", "EC45": "Airbus H145",
    "EC55": "Airbus H155", "EC75": "Airbus H175", "AS50": "Airbus H125",
    "AS55": "Airbus AS355", "AS65": "Airbus Dauphin", "B06": "Bell 206",
    "B407": "Bell 407", "B412": "Bell 412", "B429": "Bell 429", "B505": "Bell 505",
    "S76": "Sikorsky S-76", "S92": "Sikorsky S-92", "H60": "Sikorsky Black Hawk",
    "A109": "Leonardo AW109", "A139": "Leonardo AW139", "A169": "Leonardo AW169",
    "H500": "MD 500",
    # Lighter than air, gliders
    "BALL": "Balloon", "GLID": "Glider",
}


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    for code, name in TYPES.items():
        assert 1 <= len(code) <= 4, code
        assert len(name) <= MAX_NAME, (code, name, len(name))
        assert '"' not in name and "\\" not in name, name
    rows = sorted(TYPES.items())

    header = root / "include" / "data" / "aircraft_types.h"
    header.write_text(
        "#pragma once\n\n"
        "#include <cstddef>\n\n"
        "/**\n"
        " * ICAO type designator -> display name (\"B38M\" -> \"Boeing 737 MAX 8\"),\n"
        " * sorted by code for binary search. Generated by\n"
        " * scripts/build_aircraft_types.py -- edit the list there.\n"
        " */\n"
        "namespace data::aircraft_types {\n\n"
        "struct AircraftType {\n"
        "  char code[5];\n"
        "  const char* name;\n"
        "};\n\n"
        f"constexpr size_t kTypeCount = {len(rows)};\n"
        f"constexpr size_t kMaxNameLen = {MAX_NAME};\n"
        "extern const AircraftType kTypes[kTypeCount];\n\n"
        "/** Display name for an ICAO type designator, or nullptr if unknown. */\n"
        "const char* nameFor(const char* code);\n\n"
        "}  // namespace data::aircraft_types\n"
    )

    body = [
        "// Generated by scripts/build_aircraft_types.py -- do not edit.\n",
        '#include "data/aircraft_types.h"\n',
        "#include <cstring>\n",
        "namespace data::aircraft_types {\n",
        "const AircraftType kTypes[kTypeCount] = {",
    ]
    body += [f'    {{"{code}", "{name}"}},' for code, name in rows]
    body += [
        "};\n",
        "const char* nameFor(const char* code) {",
        "  if (code == nullptr || code[0] == '\\0') {",
        "    return nullptr;",
        "  }",
        "  size_t lo = 0;",
        "  size_t hi = kTypeCount;",
        "  while (lo < hi) {",
        "    const size_t mid = lo + (hi - lo) / 2;",
        "    const int cmp = strncmp(code, kTypes[mid].code, sizeof(kTypes[mid].code));",
        "    if (cmp == 0) {",
        "      return kTypes[mid].name;",
        "    }",
        "    if (cmp < 0) {",
        "      hi = mid;",
        "    } else {",
        "      lo = mid + 1;",
        "    }",
        "  }",
        "  return nullptr;",
        "}\n",
        "}  // namespace data::aircraft_types",
    ]
    (root / "src" / "data" / "aircraft_types_data.cpp").write_text("\n".join(body) + "\n")
    print(f"wrote {len(rows)} aircraft types")


if __name__ == "__main__":
    main()
