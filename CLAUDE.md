# mypbr

A CPU path tracer in C++17, grown from *Ray Tracing in One Weekend* into a
physically based renderer (NEE + MIS, GGX microfacets, BVH, meshes, HDR output).
Roadmap and history: `ROADMAP.md`. Milestone renders: `renders/NN-name.png`.

## Build, test, run

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build   # must stay warning-free (-Wall -Wextra)
ctest --test-dir build                                             # furnace_test, pdf_test, denoise_test
./build/mypbr scenes/cornell.scene -o out.png [-o out.hdr] [--save-every N] [--threads N] [--denoise [--noisy raw.png]]
tools/fetch_models.sh   # bunny + Sponza into models/external/ (gitignored), needed by bunny_cornell / sponza scenes
```

`-DMYPBR_NATIVE=OFF` drops `-march=native`. Without `-o`, PPM goes to stdout.

## Layout

Everything is header-only in `include/`; `main.cpp` parses args, loads the scene,
wraps `scene.world` in a `bvh`, renders and writes outputs. `src/stb_impl.cpp` is
the only other translation unit (stb, compiled as a SYSTEM library).

| File | What it holds |
|---|---|
| `scene.h` | `scene` struct and `load_scene()`. **The scene file format is documented in the comment at the top.** |
| `camera.h` | Camera setup, tiled multithreaded progressive render, and the integrator (`ray_color`, `sample_light`). |
| `denoiser.h` | `frame` (radiance + albedo/normal/variance guides) and `denoiser`: firefly clamp, then SVGF-style edge-avoiding à-trous filter on radiance / albedo. |
| `material.h` | `material` interface; `lambertian`, `metal`, `dielectric`, `principled`, `diffuse_light`. |
| `microfacet.h` | GGX distribution + Smith G, Fresnel, VNDF reflection sampling, Kulla-Conty multiple-scattering compensation (`ggx_albedo_table`, `ggx_ms_term`), rough dielectric BSDF. |
| `environment.h` | `environment_light`: equirectangular HDR sky, importance sampled with a 2D CDF; lives in `lights` and `camera::environment`. |
| `medium.h` | `constant_medium` (homogeneous fog in a convex boundary, free-flight sampling in `hit`) and `phase_material` (Henyey-Greenstein). |
| `pdf.h` | Direction densities: `cosine_pdf`, `hittable_pdf` (light sampling), `mixture_pdf`. |
| `hittable.h`, `hittable_list.h` | `hit_record`, `hittable` interface (incl. `pdf_value`/`random` for lights). |
| `sphere.h`, `quad.h` (+ `box()`), `mesh.h` (`triangle`, `mesh`) | Geometry. |
| `bvh.h`, `aabb.h` | Binned-SAH BVH, flattened, iterative near-first traversal. Meshes own an inner BVH. |
| `instance.h`, `transform.h` | Affine instancing: rays are moved into object space. |
| `obj_loader.h` | Minimal OBJ reader (v/vn/vt/f, fan triangulation, `usemtl`) and MTL reader (`Kd`/`map_Kd`, `Ks`/`Ns` → principled, `Ke`, alpha masks). |
| `texture.h` | `solid_color`, `checker_texture`, `image_texture` (8-bit kept as bytes, decoded from sRGB on lookup; clamp or repeat; alpha). |
| `tonemap.h`, `image_io.h`, `color3.h` | Exposure + none/reinhard/aces, sRGB curve, PNG/PPM/HDR/PFM writers. |
| `vec3.h`, `ray.h`, `interval.h`, `onb.h`, `rtweekend.h` | Math, `random_double()`, constants. |

## How a render flows

`camera::render` runs `samples_per_pixel` passes; each pass adds one sample per
pixel, with 16×16 tiles claimed by worker threads from an atomic counter. The first
`floor(sqrt(spp))²` passes are stratified. `ray_color` is iterative: at each hit it
adds emission (MIS-weighted against light sampling unless the previous bounce was
specular), then either follows a delta ray (`srec.skip_pdf`) or does NEE via
`sample_light` and samples the BSDF pdf. `beta *= eval / pdf`. Russian roulette from
depth 3. NaN/inf samples are dropped. Output: `.png`/`.ppm` get exposure + tonemap +
sRGB; `.hdr`/`.pfm` keep raw linear radiance.
With `--denoise`, `camera::collect_features` also traces each camera ray to its first
non-delta surface (`first_surface`) for albedo and normal, and sums luminance² for the
variance; `main` runs `denoiser::run` on the `frame` before every save.

## Conventions

- Style follows the RTiOW codebase: `snake_case` types (`class hit_record{`), 4-space
  indent with members at 8, include guards, `double` everywhere, `color` = `vec3`.
- New code goes in headers under `include/`; add a scene in `scenes/` to exercise it.
- Unused virtual parameters are silenced with `(void)x;` to keep `-Wextra` quiet.
- Comments explain the math or the reason, e.g. `// D * G * F / (4 cos_o)`.
- New sampling code should get a case in `tests/pdf_test.cpp` (pdf integrates to 1
  and matches `generate()`); new materials a case in `tests/furnace_test.cpp`.

## Gotchas

- `material::eval` returns **f · cos θ**, not f. `scattering_pdf` is legacy.
- A material with `skip_pdf = false` must make `eval` consistent with its `pdf_ptr`,
  or MIS gives wrong results. Delta lobes (roughness 0 metal/glass) use `skip_pdf`.
- `metal r g b x`: x is **GGX roughness** (alpha = roughness², min 1e-3), not RTiOW fuzz.
- Single-scattering GGX loses energy at high roughness. `metal` and `principled`'s specular
  layer add it back with the Kulla-Conty lobe (`ggx_ms_term`, sampled by a cosine pdf in a
  mixture), so white rough metals pass the furnace test; rough glass is not compensated and
  still only checks "never gains". The E(mu, alpha) table is built on first use (~0.3 s).
- Lights are one-sided (`diffuse_light` emits on the front face only) and must be in
  both `world` and `lights`; the scene loader does this for emissive objects.
  Transformed lights must be similarity transforms (sampling pdfs assume it).
- `hit_record::mat` is a raw pointer (avoids refcount traffic across threads); the
  owning object keeps the material alive.
- `instance::hit` does not renormalize the ray direction so `t` matches in both spaces.
- `random_double()` uses a `thread_local` mt19937: safe from workers, not reproducible.
- Media need no integrator code: `constant_medium::hit` returns a random scatter point, so a
  shadow ray stopped by fog counts as blocked (unbiased transmittance). Transform a medium's
  boundary, never the medium, or its density changes with the instance's scale.
- The environment is in `lights` but never in `world`: rays that miss see it, and `sample_light` counts a
  shadow ray that reaches the sky. Its sampling density is per pixel (piecewise constant), its lookup bilinear.
- `mesh::pdf_value` is linear in triangle count: fine for small emissive meshes only.
- Scene paths (textures, OBJ) are relative to the scene file, not the CWD; MTL and
  its texture paths are relative to the OBJ.
- Alpha-masked triangles call `random_double()` inside `hit()` (stochastic coverage);
  `hit()` must not write `rec` before that test, or a rejected hit corrupts the BVH's
  closest-so-far record.
