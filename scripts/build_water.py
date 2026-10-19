#!/usr/bin/env python3
"""Generate the water layer (sea / Great Lakes) around the radar's location.

Downloads OpenStreetMap water around a point from the Overpass API: water
areas (natural=water ways and multipolygons -- lakes, including the Great
Lakes, rivers, reservoirs) plus sea coastline. Everything is clipped to a
square around the point, simplified to well under a pixel at the closest
range, and written to src/data/water_data.cpp.

Sea coastline ways run with land on the left and water on the right, which is
how each piece of the clipped square is told apart as land or water.

Data: (c) OpenStreetMap contributors, ODbL.

Usage:
  python3 scripts/build_water.py --lat 41.9786 --lon -87.9048 [--radius-km 40]
Needs shapely. The radar only draws the layer while its configured centre is
within the generated area, so this is re-run after moving it somewhere new.
"""

from __future__ import annotations

import argparse
import json
import math
import urllib.parse
import urllib.request
from pathlib import Path

from shapely.geometry import LineString, MultiLineString, Point, Polygon, box
from shapely.ops import polygonize, split, unary_union
from shapely.ops import transform as shp_transform

ROOT = Path(__file__).resolve().parent.parent
OVERPASS_MIRRORS = (
    "https://overpass-api.de/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
)
USER_AGENT = "PlaneRadar build_water.py (+https://github.com/MatixYo/ESP32-Plane-Radar)"
KM_PER_DEG = 111.0


def overpass(query: str, timeout: int) -> dict:
    import hashlib
    cache = ROOT / ".cache" / "overpass" / (hashlib.sha1(query.encode()).hexdigest() + ".json")
    if cache.exists():
        return json.loads(cache.read_text())
    data = _overpass_fetch(query, timeout)
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps(data))
    return data


def _overpass_fetch(query: str, timeout: int) -> dict:
    last = None
    for url in OVERPASS_MIRRORS:
        req = urllib.request.Request(
            url, data=urllib.parse.urlencode({"data": query}).encode(),
            headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return json.load(resp)
        except Exception as exc:  # noqa: BLE001 - try the next mirror
            print(f"  {url}: {exc}")
            last = exc
    raise RuntimeError(f"all Overpass mirrors failed: {last}")


def fetch_coastline(s: float, w: float, n: float, e: float) -> list[list[tuple[float, float]]]:
    query = f'[out:json][timeout:90];way["natural"="coastline"]({s},{w},{n},{e});out geom;'
    data = overpass(query, 120)
    ways = []
    for el in data.get("elements", []):
        geom = el.get("geometry")
        if geom and len(geom) >= 2:
            ways.append([(p["lon"], p["lat"]) for p in geom])
    return ways


def fetch_water_areas(s: float, w: float, n: float, e: float):
    """natural=water ways and relations touching the box, full geometry."""
    query = (f'[out:json][timeout:180];(way["natural"="water"]({s},{w},{n},{e});'
             f'relation["natural"="water"]({s},{w},{n},{e}););out geom;')
    data = overpass(query, 300)
    polys = []
    for el in data.get("elements", []):
        if el["type"] == "way":
            pts = [(p["lon"], p["lat"]) for p in el.get("geometry", [])]
            if len(pts) >= 4 and pts[0] == pts[-1]:
                polys.append(Polygon(pts).buffer(0))
        elif el["type"] == "relation":
            outer, inner = [], []
            for m in el.get("members", []):
                if m.get("type") != "way" or not m.get("geometry"):
                    continue
                line = LineString([(p["lon"], p["lat"]) for p in m["geometry"]])
                (inner if m.get("role") == "inner" else outer).append(line)
            shells = unary_union(list(polygonize(unary_union(outer)))) if outer else None
            holes = unary_union(list(polygonize(unary_union(inner)))) if inner else None
            if shells is not None and not shells.is_empty:
                polys.append(shells.difference(holes) if holes is not None else shells)
    return polys


def side_points(ways, frame: Polygon, eps: float):
    """Points just right (water) and left (land) of each coastline segment."""
    water, land = [], []
    for way in ways:
        for (x0, y0), (x1, y1) in zip(way, way[1:]):
            mx, my = (x0 + x1) / 2, (y0 + y1) / 2
            dx, dy = x1 - x0, y1 - y0
            length = math.hypot(dx, dy)
            if length == 0 or not frame.contains(Point(mx, my)):
                continue
            nx, ny = dy / length * eps, -dx / length * eps  # right-hand normal
            water.append(Point(mx + nx, my + ny))
            land.append(Point(mx - nx, my - ny))
    return water, land


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lat", type=float, required=True)
    ap.add_argument("--lon", type=float, required=True)
    ap.add_argument("--radius-km", type=float, default=40.0)
    ap.add_argument("--tolerance-m", type=float, default=50.0)
    ap.add_argument("--min-area-km2", type=float, default=25.0,
                    help="keep only water bodies at least this big (the lake, not rivers)")
    ap.add_argument("--river-m", type=float, default=150.0,
                    help="drop water channels narrower than about twice this")
    ap.add_argument("--smooth-m", type=float, default=250.0,
                    help="fill harbours and piers narrower than about twice this")
    args = ap.parse_args()

    dlat = args.radius_km / KM_PER_DEG
    dlon = args.radius_km / (KM_PER_DEG * math.cos(math.radians(args.lat)))
    s, n = args.lat - dlat, args.lat + dlat
    w, e = args.lon - dlon, args.lon + dlon
    frame = box(w, s, e, n)

    ways = fetch_coastline(s, w, n, e)
    print(f"coastline ways: {len(ways)}, nodes: {sum(len(x) for x in ways)}")

    water_polys: list[Polygon] = []
    if ways:
        lines = unary_union([LineString(wy) for wy in ways]).intersection(frame)
        pieces = split(frame, lines).geoms
        water_pts, land_pts = side_points(ways, frame, eps=1e-5)
        for piece in pieces:
            wet = sum(1 for p in water_pts if piece.contains(p))
            dry = sum(1 for p in land_pts if piece.contains(p))
            if wet > dry:
                water_polys.append(piece)

    areas = fetch_water_areas(s, w, n, e)
    print(f"water areas: {len(areas)}")
    water_polys += [a.intersection(frame) for a in areas]

    # Clean-up happens in kilometres around the centre, so buffers and the
    # simplification tolerance are the same in every direction.
    cos0 = math.cos(math.radians(args.lat))
    to_km = lambda x, y, z=None: ((x - args.lon) * KM_PER_DEG * cos0,  # noqa: E731
                                  (y - args.lat) * KM_PER_DEG)
    to_deg = lambda x, y, z=None: (args.lon + x / (KM_PER_DEG * cos0),  # noqa: E731
                                   args.lat + y / KM_PER_DEG)
    polys = []
    water = unary_union(water_polys) if water_polys else None
    if water is not None and not water.is_empty:
        km = shp_transform(to_km, water)
        # Opening (shrink, then grow) cuts off channels narrower than twice
        # --river-m: rivers and canals join the lake, so an area filter alone
        # keeps them.
        r = args.river_m / 1000.0
        if r > 0:
            km = km.buffer(-r, join_style=1).buffer(r, join_style=1)
        # Big water only: a lake, not every river, canal and pond.
        geoms = list(km.geoms) if hasattr(km, "geoms") else [km]
        km = unary_union([g for g in geoms if g.area >= args.min_area_km2])
        # Closing (grow, then shrink) fills harbours, piers and breakwaters so
        # the shore reads as one clean line at radar scale.
        r = args.smooth_m / 1000.0
        if r > 0:
            km = km.buffer(r, join_style=1).buffer(-r, join_style=1)
        km = km.simplify(args.tolerance_m / 1000.0, preserve_topology=True)
        geoms = list(km.geoms) if hasattr(km, "geoms") else [km]
        for g in geoms:
            if isinstance(g, Polygon) and g.area >= args.min_area_km2:
                # No holes: islands this small are breakwaters at radar scale.
                polys.append(shp_transform(to_deg, Polygon(g.exterior)))

    def e5(v: float) -> int:
        return int(round(v * 1e5))

    out = [
        "// Generated by scripts/build_water.py -- do not edit.",
        "// Coastline (c) OpenStreetMap contributors, ODbL.",
        "",
        '#include "data/water.h"',
        "",
        "namespace data::water {",
        "",
        f"const int32_t kCenterLatE5 = {e5(args.lat)};",
        f"const int32_t kCenterLonE5 = {e5(args.lon)};",
        f"const int32_t kHalfSpanLatE5 = {e5(dlat)};",
        f"const int32_t kHalfSpanLonE5 = {e5(dlon)};",
        "",
        "namespace {",
    ]
    rings = []
    # Every ring, holes included: the radar fills even-odd across all of them.
    all_rings = [r for poly in polys for r in [poly.exterior, *poly.interiors]]
    for i, ring in enumerate(all_rings):
        pts = list(ring.coords)[:-1]
        out.append(f"const Vertex kPoly{i}[] = {{")
        out += [f"    {{{e5(y)}, {e5(x)}}}," for x, y in pts]
        out.append("};")
        rings.append((f"kPoly{i}", len(pts)))
    out.append("}  // namespace")
    out.append("")
    out.append(f"const Path kWaterPolygons[] = {{{', '.join(f'{{{nm}, {c}}}' for nm, c in rings)}}};")
    out.append(f"const size_t kWaterPolygonCount = {len(rings)};")
    out += ["", "}  // namespace data::water", ""]
    if not rings:
        out = [ln.replace("const Path kWaterPolygons[] = {};", "const Path kWaterPolygons[1] = {{nullptr, 0}};") for ln in out]
    (ROOT / "src" / "data" / "water_data.cpp").write_text("\n".join(out))
    verts = sum(c for _, c in rings)
    print(f"water polygons: {len(polys)} ({len(rings)} rings), vertices: {verts}")


if __name__ == "__main__":
    main()
