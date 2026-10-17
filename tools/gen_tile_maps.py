#!/usr/bin/env python3
"""Generates the tile textures used by scenes/normal_map.scene: a 4x4 grid of
pillow-shaped tiles with grout lines between them, each with a round stud in
its middle.

  textures/tiles_normal.png     tangent-space normal map (load it linear):
                                red along +u, green along +v, blue out of the
                                surface, each mapped from [-1, 1] to [0, 255]
  textures/tiles_roughness.png  grey roughness: smooth and rough tiles in a
                                checkerboard, rough grout
  textures/tiles_metallic.png   grey metallic: 1 on the studs, 0 elsewhere

Image row 0 is the top of the texture, at v = 1. Only the standard library is
used (the PNGs are written with zlib).

Usage: python3 tools/gen_tile_maps.py
"""
import math
import os
import struct
import zlib

SIZE = 512
TILES = 4
GROUT = 0.06    # Grout half width, in tile units
BEVEL = 0.12    # Width of the rounded tile edge
STUD = 0.16     # Stud radius
STUD_HEIGHT = 0.08
EDGE_HEIGHT = 0.05


def smoothstep(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


def height(u, v):
    """Surface height at (u, v) in [0, 1]^2, in tile units."""
    tu, tv = u * TILES, v * TILES
    fu, fv = tu - math.floor(tu), tv - math.floor(tv)
    # Distance to the nearest tile edge
    edge = min(fu, 1 - fu, fv, 1 - fv)
    h = EDGE_HEIGHT * smoothstep((edge - GROUT) / BEVEL)
    # Spherical cap stud in the middle of the tile
    r2 = (fu - 0.5) ** 2 + (fv - 0.5) ** 2
    if r2 < STUD * STUD:
        h += STUD_HEIGHT * math.sqrt(1 - r2 / (STUD * STUD))
    return h


def tile_of(u, v):
    return int(u * TILES) % TILES, int(v * TILES) % TILES


def in_grout(u, v):
    fu, fv = (u * TILES) % 1, (v * TILES) % 1
    return min(fu, 1 - fu, fv, 1 - fv) < GROUT


def in_stud(u, v):
    fu, fv = (u * TILES) % 1, (v * TILES) % 1
    return (fu - 0.5) ** 2 + (fv - 0.5) ** 2 < STUD * STUD


def write_png(path, rows, channels):
    """rows: list of bytes objects, one per image row, top first."""
    color_type = {1: 0, 3: 2}[channels]
    raw = b"".join(b"\x00" + row for row in rows)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, color_type, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", header))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def to_byte(x):
    return max(0, min(255, int(round(x * 255))))


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "textures")
    d = 0.5 / SIZE  # Central differences, half a pixel each way
    # u and v span TILES tiles, and heights are in tile units
    scale = 1.0 / TILES
    normal_rows, rough_rows, metal_rows = [], [], []
    for j in range(SIZE):
        v = 1 - (j + 0.5) / SIZE
        normal_row, rough_row, metal_row = bytearray(), bytearray(), bytearray()
        for i in range(SIZE):
            u = (i + 0.5) / SIZE
            # The surface z = h(u, v) has normal (-dh/du, -dh/dv, 1)
            dhdu = (height(u + d, v) - height(u - d, v)) / (2 * d) * scale
            dhdv = (height(u, v + d) - height(u, v - d)) / (2 * d) * scale
            nx, ny, nz = -dhdu, -dhdv, 1.0
            length = math.sqrt(nx * nx + ny * ny + nz * nz)
            normal_row += bytes(to_byte(0.5 * (c / length) + 0.5) for c in (nx, ny, nz))

            tu, tv = tile_of(u, v)
            roughness = 0.85 if in_grout(u, v) else (0.12 if (tu + tv) % 2 == 0 else 0.55)
            rough_row.append(to_byte(roughness))
            metal_row.append(255 if in_stud(u, v) else 0)
        normal_rows.append(bytes(normal_row))
        rough_rows.append(bytes(rough_row))
        metal_rows.append(bytes(metal_row))

    write_png(os.path.join(out, "tiles_normal.png"), normal_rows, 3)
    write_png(os.path.join(out, "tiles_roughness.png"), rough_rows, 1)
    write_png(os.path.join(out, "tiles_metallic.png"), metal_rows, 1)


if __name__ == "__main__":
    main()
