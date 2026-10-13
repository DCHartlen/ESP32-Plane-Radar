#!/usr/bin/env python3
"""Build the landmark line dataset (shores, islands, rivers, border) around a list of sites.

The sites come from landmark_sites.txt in the repo root (or --sites PATH): one site per
line as "name, lat, lon[, radius_km]". That file and the generated data are git-ignored,
because the geometry gives away where the sites are; see landmark_sites.example.txt.
Without generated data the firmware builds with no landmarks.

Sources:
  - Shorelines and the international border: GSHHG / WDBII at full resolution, read from
    the basemap-data-hires wheel on PyPI (basemap's own binary layout).
  - Rivers: Natural Earth 10m river centerlines (public domain), from GitHub.

Output is a list of polyline chunks with a bounding box each, so the firmware can skip
chunks outside the radar. Points are stored as int16 deltas in 1e-5 degree units (about
1 m) after each chunk's absolute first point, which keeps the table around half the size
of absolute int32 coordinates.
"""

from __future__ import annotations

import argparse
import io
import json
import math
import struct
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SITES = ROOT / "landmark_sites.txt"
# Included by src/data/landmarks_data.cpp when it exists. Git-ignored.
OUT_INC = ROOT / "src" / "data" / "landmarks_local.inc"

BASEMAP_WHEEL_JSON = "https://pypi.org/pypi/basemap-data-hires/2.0.0/json"
BASEMAP_DATA_DIR = "mpl_toolkits/basemap_data/"
NE_RIVERS_URL = (
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/"
    "ne_10m_rivers_lake_centerlines.geojson"
)

# Default radius around each site. The widest range preset's outer ring is ~33 km.
DEFAULT_RADIUS_KM = 50.0

# Lakes and islands smaller than this (km², GSHHG's own area) are left out.
MIN_POLYGON_AREA_KM2 = 2.0
# Natural Earth scalerank: lower is more important. 8 keeps the Niagara, St. Lawrence,
# St. Clair, Detroit, Grand, Ottawa and similar rivers.
MAX_RIVER_SCALERANK = 8

# Douglas-Peucker tolerance. About 1 px at the closest range preset (19 m/px).
SIMPLIFY_TOLERANCE_KM = 0.02
KM_PER_DEG = 111.0

UNITS_PER_DEG = 100_000  # 1e-5 degree
MAX_DELTA = 32767
# Long straight segments (the border across the lakes) are split so every delta fits int16.
MAX_SEGMENT_DEG = 0.3
MAX_CHUNK_POINTS = 64

# Kinds, in the order the firmware draws them (later ones on top). Keep in step with
# data::landmarks::Kind in include/data/landmarks.h.
KIND_RIVER = 0
KIND_ISLAND = 1
KIND_SHORE = 2
KIND_BORDER = 3
KIND_NAMES = {
    KIND_RIVER: "River",
    KIND_ISLAND: "Island",
    KIND_SHORE: "Shore",
    KIND_BORDER: "Border",
}

Point = tuple[float, float]  # (lon, lat) in degrees


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=300) as resp:
        return resp.read()


# Boxes (west, east, south, north) around the sites, set by main().
s_boxes: list[tuple[float, float, float, float]] = []


def load_sites(path: Path) -> list[tuple[float, float, float, float]]:
    """A box around each "name, lat, lon[, radius_km]" line; # starts a comment."""
    boxes = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) not in (3, 4):
            raise SystemExit(f"{path}:{number}: expected name, lat, lon[, radius_km]")
        lat, lon = float(fields[1]), float(fields[2])
        radius_km = float(fields[3]) if len(fields) == 4 else DEFAULT_RADIUS_KM
        dlat = radius_km / KM_PER_DEG
        dlon = radius_km / (KM_PER_DEG * math.cos(math.radians(lat)))
        boxes.append((lon - dlon, lon + dlon, lat - dlat, lat + dlat))
    if not boxes:
        raise SystemExit(f"{path}: no sites")
    return boxes


def in_region(lon: float, lat: float) -> bool:
    return any(w <= lon <= e and s <= lat <= n for w, e, s, n in s_boxes)


def runs_in_region(points: list[Point]) -> list[list[Point]]:
    """Splits a polyline into the runs inside the region, keeping one point beyond each end."""
    runs: list[list[Point]] = []
    current: list[Point] = []
    prev: Point | None = None
    for p in points:
        if in_region(*p):
            if not current and prev is not None:
                current.append(prev)
            current.append(p)
        elif current:
            current.append(p)
            runs.append(current)
            current = []
        prev = p
    if current:
        runs.append(current)
    return [r for r in runs if len(r) >= 2]


def densify(points: list[Point]) -> list[Point]:
    out = [points[0]]
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        steps = max(1, math.ceil(max(abs(x1 - x0), abs(y1 - y0)) / MAX_SEGMENT_DEG))
        for i in range(1, steps + 1):
            t = i / steps
            out.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))
    return out


def simplify(points: list[Point]) -> list[Point]:
    """Douglas-Peucker in local km, so the tolerance means the same east-west as north-south."""
    if len(points) < 3:
        return points
    cos_lat = math.cos(math.radians(points[0][1]))
    xy = [(lon * KM_PER_DEG * cos_lat, lat * KM_PER_DEG) for lon, lat in points]
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = xy[a]
        bx, by = xy[b]
        dx, dy = bx - ax, by - ay
        length = math.hypot(dx, dy)
        worst, worst_i = 0.0, -1
        for i in range(a + 1, b):
            px, py = xy[i]
            if length == 0.0:
                d = math.hypot(px - ax, py - ay)
            else:
                d = abs(dx * (ay - py) - dy * (ax - px)) / length
            if d > worst:
                worst, worst_i = d, i
        if worst > SIMPLIFY_TOLERANCE_KM:
            keep[worst_i] = True
            stack.append((a, worst_i))
            stack.append((worst_i, b))
    return [p for p, k in zip(points, keep) if k]


def read_basemap_layer(wheel: zipfile.ZipFile, name: str) -> list[tuple[int, float, list[Point]]]:
    """(level, area km², points) for every polygon or line in the layer that touches the region."""
    meta = wheel.read(f"{BASEMAP_DATA_DIR}{name}meta_f.dat").decode("ascii")
    data = wheel.read(f"{BASEMAP_DATA_DIR}{name}_f.dat")
    out = []
    for line in meta.splitlines():
        fields = line.split()
        if len(fields) < 7:
            continue
        level, area, count = int(fields[0]), float(fields[1]), int(fields[2])
        south, north = float(fields[3]), float(fields[4])
        offset, size = int(fields[5]), int(fields[6])
        if all(north < s or south > n for _, _, s, n in s_boxes):
            continue
        values = struct.unpack_from(f"<{count * 2}f", data, offset)
        assert size == count * 8
        points = list(zip(values[0::2], values[1::2]))
        if not any(in_region(*p) for p in points):
            continue
        out.append((level, area, points))
    return out


def load_gshhg() -> list[tuple[int, list[Point]]]:
    info = json.loads(fetch(BASEMAP_WHEEL_JSON))
    url = next(u["url"] for u in info["urls"] if u["filename"].endswith(".whl"))
    print(f"Downloading {url}")
    wheel = zipfile.ZipFile(io.BytesIO(fetch(url)))

    lines: list[tuple[int, list[Point]]] = []
    # GSHHG levels: 1 land (sea shore), 2 lake, 3 island in a lake, 4 pond on such an island.
    for level, area, points in read_basemap_layer(wheel, "gshhs"):
        if area < MIN_POLYGON_AREA_KM2:
            continue
        kind = KIND_SHORE if level <= 2 else KIND_ISLAND
        if points[0] != points[-1]:
            points.append(points[0])
        lines.append((kind, points))
    for _, _, points in read_basemap_layer(wheel, "countries"):
        lines.append((KIND_BORDER, points))
    return lines


def load_rivers() -> list[tuple[int, list[Point]]]:
    print(f"Downloading {NE_RIVERS_URL}")
    geojson = json.loads(fetch(NE_RIVERS_URL))
    lines: list[tuple[int, list[Point]]] = []
    for feature in geojson["features"]:
        props = feature["properties"]
        geom = feature["geometry"]
        if geom is None or props.get("featurecla") != "River":
            continue
        if (props.get("scalerank") or 99) > MAX_RIVER_SCALERANK:
            continue
        parts = [geom["coordinates"]] if geom["type"] == "LineString" else geom["coordinates"]
        for part in parts:
            lines.append((KIND_RIVER, [(float(x), float(y)) for x, y, *_ in part]))
    return lines


def to_units(deg: float) -> int:
    return int(round(deg * UNITS_PER_DEG))


def build_chunks(lines: list[tuple[int, list[Point]]]):
    chunks = []  # (kind, lat0, lon0, min_lat, min_lon, max_lat, max_lon, first_delta, count)
    deltas: list[tuple[int, int]] = []  # (dlat, dlon)
    for kind, points in sorted(lines, key=lambda kl: kl[0]):
        for run in runs_in_region(points):
            pts = [(to_units(lat), to_units(lon)) for lon, lat in densify(simplify(run))]
            # Drop repeats left by rounding.
            pts = [p for i, p in enumerate(pts) if i == 0 or p != pts[i - 1]]
            if len(pts) < 2:
                continue
            # Consecutive chunks share an end point so the line has no gaps.
            for start in range(0, len(pts) - 1, MAX_CHUNK_POINTS - 1):
                part = pts[start : start + MAX_CHUNK_POINTS]
                lats = [p[0] for p in part]
                lons = [p[1] for p in part]
                chunks.append(
                    (kind, part[0][0], part[0][1], min(lats), min(lons), max(lats), max(lons),
                     len(deltas), len(part))
                )
                for (lat0, lon0), (lat1, lon1) in zip(part, part[1:]):
                    dlat, dlon = lat1 - lat0, lon1 - lon0
                    assert abs(dlat) <= MAX_DELTA and abs(dlon) <= MAX_DELTA
                    deltas.append((dlat, dlon))
    return chunks, deltas


def write_inc(chunks, deltas) -> None:
    lines = [
        "// Generated by scripts/build_landmarks.py — do not edit. Git-ignored: it shows where",
        "// the sites are. Shorelines and border: GSHHG/WDBII (LGPL). Rivers: Natural Earth.",
        "",
        "const Chunk kChunkData[] = {",
    ]
    for kind, lat0, lon0, min_lat, min_lon, max_lat, max_lon, first, count in chunks:
        lines.append(
            f"  {{{lat0}, {lon0}, {min_lat}, {min_lon}, {max_lat}, {max_lon}, {first}, "
            f"{count}, Kind::{KIND_NAMES[kind]}}},"
        )
    lines += ["};", "", "const Delta kDeltaData[] = {"]
    for i in range(0, len(deltas), 8):
        row = " ".join(f"{{{a}, {b}}}," for a, b in deltas[i : i + 8])
        lines.append(f"  {row}")
    lines += ["};", ""]
    OUT_INC.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--sites", type=Path, default=DEFAULT_SITES,
                        help="site list (default: landmark_sites.txt in the repo root)")
    args = parser.parse_args()
    if not args.sites.exists():
        raise SystemExit(f"{args.sites} not found; copy landmark_sites.example.txt and edit it")
    s_boxes.extend(load_sites(args.sites))

    lines = load_gshhg() + load_rivers()
    chunks, deltas = build_chunks(lines)
    write_inc(chunks, deltas)
    per_kind = {name: 0 for name in KIND_NAMES.values()}
    for chunk in chunks:
        per_kind[KIND_NAMES[chunk[0]]] += chunk[8] - 1
    flash_kb = (len(chunks) * 32 + len(deltas) * 4) / 1024
    print(f"Wrote {len(s_boxes)} sites: {len(chunks)} chunks, {len(deltas)} deltas "
          f"(~{flash_kb:.0f} KB flash)")
    print("Segments per kind:", per_kind)
    print(f"  {OUT_INC}")


if __name__ == "__main__":
    main()
