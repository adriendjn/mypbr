#!/usr/bin/env python3
"""Renders the validation cases with mypbr and Mitsuba 3 and compares them.

For every case in cases/, mypbr renders the .scene to a .pfm and Mitsuba
renders the same scene (translated by scene_to_mitsuba.py) to an .exr. Both
are linear radiance, so they should agree up to Monte Carlo noise; the
comparison separates the two:

  ratio       mypbr / Mitsuba of the total radiance, per channel. An energy
              error shows here even when it is smaller than the noise of any
              one pixel.
  rel. RMSE   root mean square difference over the mean pixel value. Mostly
              noise; useful to compare runs, not to judge a match on its own.
  FLIP        mean HDR-FLIP error (NVIDIA's perceptual difference metric,
              0 = identical). Noise counts too.
  bad blocks  16 x 16 pixel blocks whose mean differs by more than 3% AND by
              more than 4 standard errors of that mean: a local difference too
              large to be noise (a misplaced highlight, a wrong Fresnel at
              grazing angles, a misoriented environment).

A case matches when the total differs by less than 1% (or not significantly)
and there are no bad blocks. Results go to out/ (gitignored): the two renders,
the Mitsuba XML, a comparison strip (mypbr | Mitsuba | relative difference |
FLIP) and report.md.

Usage: validate.py [case ...] [--mypbr build/mypbr] [--spp N] [--mitsuba-spp N] [--reuse]
"""

import argparse
import math
import os
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))

# name, scene, BSDF translation (see scene_to_mitsuba.py), what it checks, expected outcome
CASES = [
    ("cornell", "cornell", "native",
     "Lambertian, area light (NEE + MIS), quads, rotated boxes, indirect bounces", "match"),
    ("metal-model", "metal", "mypbr",
     "GGX metal, roughness 0.15 to 0.9, incl. Kulla-Conty, against the same model in Mitsuba", "match"),
    ("metal-single-scatter", "metal", "mypbr-ss",
     "the same model without Kulla-Conty: shows the energy the compensation adds", "differs"),
    ("metal-native", "metal", "native",
     "GGX metal against Mitsuba's principled (separable G, no energy compensation)", "differs"),
    ("principled-model", "principled", "mypbr",
     "principled dielectric + metallic blend against the same model in Mitsuba", "match"),
    ("principled-native", "principled", "native",
     "principled against Mitsuba's principled (Disney diffuse)", "differs"),
    ("glass", "glass", "native",
     "smooth and rough dielectric, mirror", "match"),
    ("colored-glass", "colored_glass", "native",
     "Beer-Lambert glass against a homogeneous absorbing medium (volpath)", "match"),
    ("hdri", "hdri", "native",
     "HDR environment: orientation, rotation, importance sampling", "match"),
]


# --- Images ---

def read_pfm(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"PF":
            raise ValueError(f"{path}: not a color PFM")
        width, height = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.fromfile(f, dtype="<f4" if scale < 0 else ">f4", count=width * height * 3)
    return np.flipud(data.reshape(height, width, 3)).astype(np.float64)  # stored bottom row first


def luminance(img):
    return img @ np.array([0.2126, 0.7152, 0.0722])


def to_display(img, exposure):
    # Reinhard on a fixed exposure (shared by both images), then sRGB
    x = img * exposure
    x = x / (1 + x)
    x = np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(np.maximum(x, 0), 1 / 2.4) - 0.055)
    return (np.clip(x, 0, 1) * 255 + 0.5).astype(np.uint8)


def diverging(rel, limit=0.2):
    # Blue where mypbr is darker, red where it is brighter, white where equal
    t = np.clip(rel / limit, -1, 1)[..., None]
    white, red, blue = np.ones(3), np.array([0.8, 0.1, 0.1]), np.array([0.1, 0.3, 0.85])
    c = np.where(t > 0, white + t * (red - white), white - t * (blue - white))
    return (c * 255 + 0.5).astype(np.uint8)


def box_blur(img, k):
    # k x k box filter (edges clamped) on an H x W array
    pad = k // 2
    p = np.pad(img, ((pad, k - 1 - pad), (pad, k - 1 - pad)), mode="edge")
    c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


# --- Comparison ---

def compare(test, ref, block=16):
    stats = {}
    stats["ratio"] = test.reshape(-1, 3).sum(0) / np.maximum(ref.reshape(-1, 3).sum(0), 1e-12)
    stats["rel_rmse"] = math.sqrt(np.mean((test - ref) ** 2)) / max(ref.mean(), 1e-12)

    lt, lr = luminance(test), luminance(ref)
    d = lt - lr
    n = d.size
    stats["total_rel"] = d.mean() / max(lr.mean(), 1e-12)
    stats["total_z"] = d.mean() / max(d.std() / math.sqrt(n), 1e-12)

    # Blocks whose difference is both large and statistically significant
    h, w = d.shape
    bad, worst = [], 0.0
    for y in range(0, h - block + 1, block):
        for x in range(0, w - block + 1, block):
            db = d[y:y+block, x:x+block]
            ref_mean = lr[y:y+block, x:x+block].mean()
            if ref_mean < 0.01 * lr.mean():
                continue  # Black regions: a relative error means nothing
            rel = db.mean() / ref_mean
            z = db.mean() / max(db.std() / math.sqrt(db.size), 1e-12)
            if abs(rel) > 0.03 and abs(z) > 4:
                bad.append((y, x, rel, z))
                if abs(rel) > abs(worst):
                    worst = rel
    stats["bad_blocks"] = bad
    stats["blocks"] = (h // block) * (w // block)
    stats["worst_block"] = worst

    import flip_evaluator as flip
    flip_map, flip_mean, _ = flip.evaluate(ref.astype(np.float32), test.astype(np.float32), "HDR")
    stats["flip"] = float(flip_mean)
    stats["flip_map"] = flip_map

    significant = abs(stats["total_z"]) > 4
    stats["match"] = not bad and (abs(stats["total_rel"]) < 0.01 or not significant)
    return stats


def save_strip(path, test, ref, stats):
    from PIL import Image
    exposure = 0.5 / max(np.percentile(luminance(ref), 90), 1e-6)
    lt, lr = box_blur(luminance(test), 5), box_blur(luminance(ref), 5)
    rel = (lt - lr) / np.maximum(lr, 1e-3 * lr.mean())
    flip_map = stats["flip_map"]
    if flip_map.dtype != np.uint8:
        flip_map = (np.clip(flip_map, 0, 1) * 255 + 0.5).astype(np.uint8)
    if flip_map.ndim == 2:
        flip_map = np.repeat(flip_map[..., None], 3, axis=2)
    gap = np.full((ref.shape[0], 4, 3), 255, np.uint8)
    strip = np.concatenate([to_display(test, exposure), gap, to_display(ref, exposure), gap,
                            diverging(rel), gap, flip_map[..., :3]], axis=1)
    Image.fromarray(strip).save(path)


# --- Rendering ---

def render_mypbr(exe, scene_path, out_path, spp=None):
    path = scene_path
    if spp:
        # mypbr reads spp from the scene: render a copy next to the original
        # (relative paths in it stay valid)
        path = os.path.join(os.path.dirname(scene_path), f".spp{spp}-" + os.path.basename(scene_path))
        with open(scene_path) as f, open(path, "w") as g:
            for line in f:
                g.write(f"samples_per_pixel {spp}\n" if line.split()[:1] == ["samples_per_pixel"] else line)
    try:
        subprocess.run([exe, path, "-o", out_path], check=True, stderr=subprocess.DEVNULL)
    finally:
        if path != scene_path:
            os.remove(path)


def render_mitsuba(xml, spp, out_path, chunk=256):
    import mitsuba as mi
    scene = mi.load_string(xml)
    total, done, seed = None, 0, 0
    while done < spp:
        n = min(chunk, spp - done)  # Chunks keep the wavefront arrays small
        img = np.array(mi.render(scene, spp=n, seed=seed), dtype=np.float64)
        total = img * n if total is None else total + img * n
        done += n
        seed += 1
    img = total / spp
    mi.Bitmap(img.astype(np.float32)).write(out_path)
    return img


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("cases", nargs="*", help="case names (default: all)")
    parser.add_argument("--mypbr", default=os.path.join(REPO, "build", "mypbr"))
    parser.add_argument("--spp", type=int, help="mypbr samples per pixel (default: the scene's)")
    parser.add_argument("--mitsuba-spp", type=int, help="Mitsuba samples per pixel (default: 4x mypbr's)")
    parser.add_argument("--out", default=os.path.join(HERE, "out"))
    parser.add_argument("--reuse", action="store_true", help="keep renders already in --out")
    parser.add_argument("--variant", default="llvm_ad_rgb", help="Mitsuba variant (scalar_rgb without LLVM)")
    args = parser.parse_args()

    import mitsuba as mi
    mi.set_variant(args.variant)
    sys.path.insert(0, HERE)
    import mypbr_bsdf  # noqa: F401  (registers mypbr_principled)
    import scene_to_mitsuba as s2m

    selected = [c for c in CASES if not args.cases or c[0] in args.cases]
    if args.cases and len(selected) != len(args.cases):
        sys.exit(f"unknown case; known: {', '.join(c[0] for c in CASES)}")
    os.makedirs(args.out, exist_ok=True)

    rows = []
    for name, scene_name, bsdfs, checks, expected in selected:
        scene_path = os.path.join(HERE, "cases", scene_name + ".scene")
        case_dir = os.path.join(args.out, name)
        os.makedirs(case_dir, exist_ok=True)
        sc = s2m.load(scene_path)
        spp = args.spp or sc.spp
        mitsuba_spp = args.mitsuba_spp or 4 * spp

        # One mypbr render per scene, shared by its cases
        pfm = os.path.join(args.out, f"{scene_name}.mypbr.pfm")
        if not (args.reuse and os.path.exists(pfm)):
            t = time.time()
            print(f"[{name}] mypbr, {spp} spp ...", flush=True)
            render_mypbr(args.mypbr, scene_path, pfm, args.spp)
            print(f"[{name}] mypbr done in {time.time() - t:.0f} s", flush=True)
        test = read_pfm(pfm)

        exr = os.path.join(case_dir, "mitsuba.exr")
        xml = s2m.to_xml(sc, bsdfs, mitsuba_spp)
        with open(os.path.join(case_dir, "scene.xml"), "w") as f:
            f.write(xml)
        if args.reuse and os.path.exists(exr):
            ref = np.array(mi.Bitmap(exr), dtype=np.float64)[..., :3]
        else:
            t = time.time()
            print(f"[{name}] Mitsuba ({bsdfs} BSDFs), {mitsuba_spp} spp ...", flush=True)
            ref = render_mitsuba(xml, mitsuba_spp, exr)
            print(f"[{name}] Mitsuba done in {time.time() - t:.0f} s", flush=True)

        stats = compare(test, ref)
        save_strip(os.path.join(case_dir, "compare.png"), test, ref, stats)
        verdict = "match" if stats["match"] else "DIFFERS"
        r = stats["ratio"]
        print(f"[{name}] {verdict}: ratio {r[0]:.4f} {r[1]:.4f} {r[2]:.4f}, rel. RMSE {stats['rel_rmse']:.4f}, "
              f"FLIP {stats['flip']:.4f}, bad blocks {len(stats['bad_blocks'])}/{stats['blocks']}"
              + (f" (worst {stats['worst_block']:+.1%})" if stats["bad_blocks"] else ""), flush=True)
        rows.append((name, checks, expected, verdict, stats))

    with open(os.path.join(args.out, "report.md"), "w") as f:
        f.write("# mypbr vs Mitsuba 3\n\n")
        f.write("Each strip: mypbr | Mitsuba | relative difference (red: mypbr brighter, blue: darker, "
                "full color at 20%) | FLIP error.\n\n")
        f.write("| case | checks | expected | result | ratio (r g b) | rel. RMSE | FLIP | bad blocks |\n")
        f.write("|---|---|---|---|---|---|---|---|\n")
        for name, checks, expected, verdict, s in rows:
            r = s["ratio"]
            worst = f" (worst {s['worst_block']:+.1%})" if s["bad_blocks"] else ""
            f.write(f"| {name} | {checks} | {expected} | {verdict} | {r[0]:.3f} {r[1]:.3f} {r[2]:.3f} | "
                    f"{s['rel_rmse']:.3f} | {s['flip']:.3f} | {len(s['bad_blocks'])}/{s['blocks']}{worst} |\n")
        f.write("\n")
        for name, *_ in rows:
            f.write(f"## {name}\n\n![{name}]({name}/compare.png)\n\n")
    print(f"Report: {os.path.join(args.out, 'report.md')}")

    unexpected = [n for n, _, expected, verdict, _ in rows if (verdict == "match") != (expected == "match")]
    if unexpected:
        print("Unexpected results: " + ", ".join(unexpected))
    return 1 if unexpected else 0


if __name__ == "__main__":
    sys.exit(main())
