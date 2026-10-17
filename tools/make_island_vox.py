#!/usr/bin/env python3
"""Writes scenes/models/island.vox, a small Cube World-style test island.

It exercises what the .vox loader reads: several models placed by the scene
graph (a tree model instanced three times with rotations), a custom palette,
and MATL materials (glass water, an emissive lantern, a metal bell).

Run from anywhere: python3 tools/make_island_vox.py
The output is deterministic (fixed random seed).
"""
import math
import os
import random
import struct


def chunk(cid, content, children=b""):
    return cid + struct.pack("<ii", len(content), len(children)) + content + children


def string(s):
    b = s.encode()
    return struct.pack("<i", len(b)) + b


def dictionary(d):
    out = struct.pack("<i", len(d))
    for k, v in d.items():
        out += string(k) + string(str(v))
    return out


def model(size, voxels):
    xyzi = struct.pack("<i", len(voxels)) + b"".join(struct.pack("4B", *v) for v in voxels)
    return chunk(b"SIZE", struct.pack("<3i", *size)) + chunk(b"XYZI", xyzi)


# Palette indices (1-based) and their colors
GRASS, GRASS2, DIRT, STONE, SAND, WATER, BARK, LEAF, LEAF2, LANTERN, BELL, POST = range(1, 13)
COLORS = {
    GRASS: (106, 190, 48), GRASS2: (90, 168, 42), DIRT: (138, 94, 60), STONE: (128, 128, 136),
    SAND: (230, 210, 150), WATER: (70, 150, 220), BARK: (110, 72, 40), LEAF: (60, 150, 60),
    LEAF2: (80, 175, 70), LANTERN: (255, 190, 90), BELL: (220, 180, 80), POST: (70, 60, 50),
}


def terrain():
    rng = random.Random(7)
    n, h = 64, 24
    bumps = [(rng.uniform(8, 56), rng.uniform(8, 56), rng.uniform(4, 9), rng.uniform(3, 7)) for _ in range(9)]
    vox = []
    water_level = 5
    for x in range(n):
        for y in range(n):
            dx, dy = (x - n / 2) / (n / 2), (y - n / 2) / (n / 2)
            r = math.hypot(dx, dy)
            height = 9 * (1 - r * r) + sum(a * math.exp(-((x - bx) ** 2 + (y - by) ** 2) / (2 * s * s))
                                           for bx, by, a, s in bumps)
            top = int(height)
            for z in range(max(top, 0)):
                if z == top - 1:
                    idx = SAND if top <= water_level + 1 else (GRASS if (x * 7 + y * 13) % 5 else GRASS2)
                elif z >= top - 3:
                    idx = DIRT
                else:
                    idx = STONE
                vox.append((x, y, z, idx))
            for z in range(max(top, 0), water_level):
                vox.append((x, y, z, WATER))
    # A lantern post with an emissive lamp and a metal bell hanging under it
    px, py = 40, 26
    base = max(z for x, y, z, _ in vox if (x, y) == (px, py)) + 1
    for z in range(base, base + 6):
        vox.append((px, py, z, POST))
    vox.append((px + 1, py, base + 5, POST))
    vox.append((px + 2, py, base + 5, LANTERN))
    vox.append((px + 2, py, base + 4, BELL))
    return (n, n, h), vox


def tree():
    vox = []
    for z in range(0, 7):
        vox.append((3, 3, z, BARK))
    for x in range(7):
        for y in range(7):
            for z in range(5, 11):
                if (x - 3) ** 2 + (y - 3) ** 2 + 1.5 * (z - 7.5) ** 2 <= 11:
                    if (x, y) != (3, 3) or z > 6:
                        vox.append((x, y, z, LEAF if (x + y + z) % 3 else LEAF2))
    return (7, 7, 11), vox


def main():
    models = [terrain(), tree()]
    body = model(*models[0]) + model(*models[1])

    # Scene graph: root transform 0 -> group 1 -> transforms 2, 4, 6, 8 ->
    # shapes 3 (terrain), 5, 7, 9 (trees, rotated by the _r byte)
    def trn(node, child, t, r=None):
        frame = {"_t": " ".join(str(v) for v in t)}
        if r is not None:
            frame["_r"] = r
        content = struct.pack("<i", node) + dictionary({}) + struct.pack("<iiii", child, -1, 0, 1) + dictionary(frame)
        return chunk(b"nTRN", content)

    def shp(node, model_id):
        return chunk(b"nSHP", struct.pack("<i", node) + dictionary({}) + struct.pack("<ii", 1, model_id) + dictionary({}))

    graph = trn(0, 1, (0, 0, 0))
    children = [2, 4, 6, 8]
    graph += chunk(b"nGRP", struct.pack("<i", 1) + dictionary({}) + struct.pack("<i", len(children))
                   + b"".join(struct.pack("<i", c) for c in children))
    graph += trn(2, 3, (0, 0, 12)) + shp(3, 0)
    # Trees: the terrain is centered at the origin; its top is 12 + height - 24 / 2
    terrain_vox = models[0][1]

    def ground(x, y):
        return max(z for vx, vy, z, _ in terrain_vox if (vx, vy) == (x, y)) + 1

    # _r 4 is the identity (row 0 -> column 0, row 1 -> column 1); 17 turns
    # 90 degrees about z (row 0 -> -y, row 1 -> x)
    for node, (x, y, r) in zip((4, 6, 8), ((22, 24, 4), (30, 40, 17), (44, 38, 4))):
        graph += trn(node, node + 1, (x - 32, y - 32, ground(x, y) + 5), r) + shp(node + 1, 1)

    rgba = bytearray()
    for k in range(1, 257):  # 256 entries; entry k is palette index k (the last is unused)
        rgba += bytes(COLORS.get(k, (200, 200, 200))) + b"\xff"
    body += graph + chunk(b"RGBA", bytes(rgba))
    for idx, props in ((WATER, {"_type": "_glass", "_rough": "0", "_ri": "1.33", "_att": "0.3"}),
                       (LANTERN, {"_type": "_emit", "_emit": "1", "_flux": "1.3"}),
                       (BELL, {"_type": "_metal", "_metal": "1", "_rough": "0.25"})):
        body += chunk(b"MATL", struct.pack("<i", idx) + dictionary(props))

    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scenes", "models", "island.vox")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"VOX " + struct.pack("<i", 200) + chunk(b"MAIN", b"", body))
    print("wrote", os.path.normpath(path))


if __name__ == "__main__":
    main()
