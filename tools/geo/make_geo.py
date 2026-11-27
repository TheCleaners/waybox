#!/usr/bin/env python3
"""Make wbconf's location picker data (data/geo/) from Natural Earth.

Natural Earth (naturalearthdata.com) is public domain. From release
v5.1.2, pinned, so the files come out the same each time:

  countries.bin  Country outlines at 1:110m: "WBGEO1\\n", then for each
                 ring a uint32 point count and that many int16 pairs,
                 longitude and latitude in hundredths of a degree, all
                 little-endian.
  cities.tsv     The populated places (about 7,300): name, country,
                 latitude, longitude, population, tab-separated, one a
                 line, the most populous first.

Usage: tools/geo/make_geo.py [output-dir]   (default: data/geo)
"""
import json
import os
import struct
import sys
import urllib.request

RELEASE = "v5.1.2"
BASE = ("https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
        + RELEASE + "/geojson/")


def fetch(name):
    with urllib.request.urlopen(BASE + name + ".geojson", timeout=60) as r:
        return json.load(r)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "data", "geo")
    os.makedirs(out, exist_ok=True)

    countries = fetch("ne_110m_admin_0_countries")
    blob = bytearray(b"WBGEO1\n")
    rings = 0
    for f in countries["features"]:
        g = f["geometry"]
        polys = g["coordinates"] if g["type"] == "MultiPolygon" else [g["coordinates"]]
        for poly in polys:
            for ring in poly:
                blob += struct.pack("<I", len(ring))
                for lon, lat in ring:
                    blob += struct.pack("<hh", round(lon * 100), round(lat * 100))
                rings += 1
    with open(os.path.join(out, "countries.bin"), "wb") as fh:
        fh.write(blob)

    places = fetch("ne_10m_populated_places_simple")
    rows = []
    for f in places["features"]:
        p = f["properties"]
        lon, lat = f["geometry"]["coordinates"]
        name = (p.get("name") or "").replace("\t", " ").strip()
        country = (p.get("adm0name") or p.get("sov0name") or "").replace("\t", " ").strip()
        if not name:
            continue
        pop = int(p.get("pop_max") or 0)
        rows.append((pop, name, country, lat, lon))
    rows.sort(key=lambda r: (-r[0], r[1]))
    with open(os.path.join(out, "cities.tsv"), "w", encoding="utf-8") as fh:
        for pop, name, country, lat, lon in rows:
            fh.write(f"{name}\t{country}\t{lat:.2f}\t{lon:.2f}\t{pop}\n")
    print(f"{out}: {rings} rings, {len(blob)} bytes; {len(rows)} cities")


if __name__ == "__main__":
    main()
