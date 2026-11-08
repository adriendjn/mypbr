# mypbr

A CPU path tracer in C++17, grown from *Ray Tracing in One Weekend* into a
physically based renderer (NEE + MIS, GGX microfacets, BVH, meshes, HDR output).
Roadmap and history: `ROADMAP.md`. Milestone renders: `renders/NN-name.png`.

## Status

Milestones 1-10 of `ROADMAP.md` are done, plus 11 (HDRI environment, MTL textures and
alpha masks, fog, denoiser) and 12 (normal maps, textured roughness/metallic, colored
glass, Kulla-Conty compensation, power-based light selection) and 13 (MagicaVoxel look:
.vox loader, DDA voxel grid, sun, atmospheric sky, ground, bloom/vignette/bokeh, ortho
camera, height fog, `style voxel` preset). Milestone 14 (cars) has started: glTF 2.0 loading
(`mesh car.glb`), thin glass, `scenes/car_*.scene` (the Porsches come from Sketchfab: `SKETCHFAB_TOKEN` in a git-ignored `.env`), clearcoat (`principled` coat layer, KHR_materials_clearcoat), a photo studio (`cyclorama`, `softbox`, gridded lights `emissive ... spread`; `scenes/car_studio.scene`), measured metals (exact conductor Fresnel from a complex IOR: `metal Al 0.3`, principled `conductor Ti`) and tinted clear coats (`clearcoat_color`; the studio car's copper-painted aluminium rims). Known gaps: grey-scale
bump/height maps are skipped with a warning, rough glass is not energy-compensated,
the denoiser's normal guide ignores normal maps. The BSDFs, area lights and HDRI are
validated against Mitsuba 3 (`tools/mitsuba/`, results in its README).
`scenes/showcase.scene` puts most features in one HDRI still life (needs the bunny).

## Build, test, run

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build   # must stay warning-free (-Wall -Wextra)
ctest --test-dir build                                             # furnace_test, pdf_test, denoise_test, voxel_test
./build/mypbr scenes/cornell.scene -o out.png [-o out.hdr] [--save-every N] [--threads N] [--denoise [--noisy raw.png]]
tools/fetch_models.sh   # bunny, Sponza, .vox samples into models/external/ (gitignored), needed by bunny_cornell / sponza / vox_* scenes
python3 tools/mitsuba/validate.py [case...]   # compare with Mitsuba 3 (pip install mitsuba flip-evaluator numpy pillow)
```

`-DMYPBR_NATIVE=OFF` drops `-march=native`. Without `-o`, PPM goes to stdout.

## Layout

Everything is header-only in `include/`; `main.cpp` parses args, loads the scene,
wraps `scene.world` in a `bvh`, renders and writes outputs. `src/stb_impl.cpp` is
the only other translation unit (stb and cgltf, compiled as a SYSTEM library).

| File | What it holds |
|---|---|
| `scene.h` | `scene` struct and `load_scene()`. **The scene file format is documented in the comment at the top.** |
| `camera.h` | Camera setup, tiled multithreaded progressive render, and the integrator (`ray_color`, `sample_light`). |
| `denoiser.h` | `frame` (radiance + albedo/normal/variance guides) and `denoiser`: firefly clamp, then SVGF-style edge-avoiding à-trous filter on radiance / albedo. |
| `material.h` | `material` interface; `lambertian`, `metal`, `dielectric` (optional Beer-Lambert tint), `principled` (metallic/roughness textures, own base normal map, optional clear coat with its own roughness, normal map and tint, optional measured metal), `diffuse_light` (optional honeycomb-grid `spread`), `normal_mapped` (wrapper that bends the shading normal), `thin_dielectric` (glass sheet: reflect or pass straight; scene type `thin_glass`), `emissive_surface` (textured glow on another material, not a light). |
| `microfacet.h` | GGX distribution + Smith G, Fresnel (Schlick, exact dielectric, exact conductor; `find_conductor` table of RGB complex IORs, `metal_fresnel`), VNDF reflection sampling, Kulla-Conty multiple-scattering compensation (`ggx_albedo_table`, `ggx_ms_term`), rough dielectric BSDF. |
| `environment.h` | `environment_light`: equirectangular HDR sky, importance sampled with a 2D CDF; lives in `lights` and `camera::environment`. |
| `medium.h` | `constant_medium` (homogeneous fog in a convex boundary, free-flight sampling in `hit`) and `phase_material` (Henyey-Greenstein). |
| `pdf.h` | Direction densities: `cosine_pdf`, `hittable_pdf` (light sampling), `mixture_pdf`. |
| `light_list.h` | `light_list` (`scene.lights`): picks a light by power (`hittable::power()`, 20% kept uniform) from a CDF; `pdf_value` = sum p_k pdf_k. Scene keyword `light_selection power\|uniform`. |
| `hittable.h`, `hittable_list.h` | `hit_record`, `hittable` interface (incl. `pdf_value`/`random` for lights). |
| `sphere.h`, `quad.h` (+ `box()`), `mesh.h` (`triangle`, `mesh`) | Geometry. |
| `bvh.h`, `aabb.h` | Binned-SAH BVH, flattened, iterative near-first traversal. Meshes own an inner BVH. |
| `instance.h`, `transform.h` | Affine instancing: rays are moved into object space. |
| `obj_loader.h` | Minimal OBJ reader (v/vn/vt/f, fan triangulation, `usemtl`) and MTL reader (`Kd`/`map_Kd`, `Ks`/`Ns` → principled, `Ke`, alpha masks). |
| `texture.h` | `solid_color`, `checker_texture`, `image_texture` (8-bit kept as bytes, decoded from sRGB on lookup; clamp or repeat; alpha; from a file or memory); wrappers `scaled_texture`, `channel_texture`, `uv_transform_texture`, `normal_scale_texture` (for glTF). |
| `tonemap.h`, `image_io.h`, `color3.h` | Exposure + none/reinhard/aces, sRGB curve, PNG/PPM/HDR/PFM writers. |
| `postprocess.h` | Bloom (threshold + blur pyramid) and vignette, on display images only (`display_settings::post`). |
| `gltf_loader.h` | glTF 2.0 (.gltf/.glb) through cgltf (`external/cgltf.h`): node tree flattened into world-space triangles, metallic-roughness materials → `principled` (+ clearcoat), transmission → `thin_dielectric`, emission, texture transforms. |
| `vox_loader.h` | MagicaVoxel `.vox` reader: models, palette, MATL, nTRN/nGRP/nSHP scene graph (still z-up). |
| `voxel.h` | `voxel_palette` (MATL → materials, `voxel_kind`), `voxel_model_faces` (quads, reference), `voxel_grid` (3D DDA over a block) and `voxel_blocks` (8³ blocks in a BVH). |
| `sun.h` | `sun_light`: distant cone light given by irradiance; in `lights` and `camera::sun`, never in `world`. |
| `atmosphere.h` | Single-scattering Rayleigh/Mie/ozone sky, baked into an `environment_light`; also reddens the sun. |
| `studio.h` | `make_cyclorama` (floor curving into a wall, one mesh with smooth normals) and `make_softbox` (a quad light centered on a point, facing another). |
| `plane.h` | `ground_plane`: infinite plane, kept in `scene::unbounded` (beside the BVH). |
| `vec3.h`, `ray.h`, `interval.h`, `onb.h`, `rtweekend.h` | Math, `random_double()`, constants. |

## How a render flows

`camera::render` runs `samples_per_pixel` passes; each pass adds one sample per
pixel, with 16×16 tiles claimed by worker threads from an atomic counter. The first
`floor(sqrt(spp))²` passes are stratified. `main` tests `scene::unbounded` (ground, height fog)
beside the BVH. `ray_color` is iterative: at each hit it
adds emission (MIS-weighted against light sampling unless the previous bounce was
specular), then either follows a delta ray (`srec.skip_pdf`) or does NEE via
`sample_light` and samples the BSDF pdf. `beta *= eval / pdf`. Russian roulette from
depth 3, and the optional per-kind caps (`bounces`). Rays that escape see
`escaped_radiance` (environment or gradient or background, plus the sun). NaN/inf samples
are dropped. Output: `.png`/`.ppm` get bloom + vignette + exposure + tonemap + sRGB;
`.hdr`/`.pfm` keep raw linear radiance.
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
- Smooth glass uses exact Fresnel (`fresnel_dielectric`), not Schlick: Schlick with the incident
  angle stays near 4% inside the glass up to total internal reflection, darkening rims ~10%.
- `tools/mitsuba/scene_to_mitsuba.py` refuses features it can't translate (an error, not an
  approximation); extend it when a new material should be validated. `principled` is 2-3% darker overall
  than Mitsuba's native principled (up to 23% at rims) by design ((1 - F)(1 - F) base), not a bug.
- Shadow rays (`sample_light`) use a **normalized** direction so the 0.001 self-hit offset is a
  distance; with the raw `to_light` vector it scaled with the distance to the light.
- `pdf_test`'s full-sphere environment sampling check has a wide tolerance (0.4 on 4π) because
  its estimate has σ ≈ 0.08; a tighter bound fails randomly, it does not signal a bug.
- `map_Bump`/`bump`/`map_Disp`/`disp` in MTL are read as normal maps when the image has color
  (Sponza's `*_ddn.tga`); grey-scale height maps are skipped.
- Lights are one-sided (`diffuse_light` emits on the front face only) and must be in
  both `world` and `lights`; the scene loader does this for emissive objects.
  Transformed lights must be similarity transforms (sampling pdfs assume it).
- `hit_record::mat` is a raw pointer (avoids refcount traffic across threads); the
  owning object keeps the material alive.
- `instance::hit` does not renormalize the ray direction so `t` matches in both spaces.
- `random_double()` uses a `thread_local` mt19937: safe from workers, not reproducible.
- Voxel surfaces exist only between cells of different `voxel_kind` (opaque, transparent glass,
  empty): adjacent glass voxels are one volume. `voxel_grid` blocks read neighbours from the
  whole model, so a boundary surface is found by both blocks at the same t. Change the rules
  in `voxel_face_visible` only, and keep `voxel_test` passing (grid = quads).
- Emissive voxels are lights through their faces as quads in `lights` (the grid itself is hit):
  like `mesh::pdf_value`, cost is linear in emissive face count.
- `.vox` files are z-up; `vox` turns them with rotate_x -90 before the line's own transforms.
- `transparent_shadows` and `bounces` are deliberate biases (MagicaVoxel's look): off unless
  set or `style voxel`. With transparent shadows, a path refracted through smooth glass after a
  non-specular vertex drops the emission it reaches, since that vertex's shadow ray counted it.
- `sky atmosphere` uses the sun's settings even with `sun off`, and its `intensity` defaults to 3
  (single scattering alone is about 3x too dark).
- Media need no integrator code: `constant_medium::hit` returns a random scatter point, so a
  shadow ray stopped by fog counts as blocked (unbiased transmittance). Transform a medium's
  boundary, never the medium, or its density changes with the instance's scale.
- The environment is in `lights` but never in `world`: rays that miss see it, and `sample_light` counts a
  shadow ray that reaches the sky. Its sampling density is per pixel (piecewise constant), its lookup bilinear.
- Normal maps need `hit_record::dpdu`/`dpdv`: new geometry must set them, or the unperturbed normal is used.
  Normal-map and roughness/metallic images must be loaded `linear` (no sRGB decode). `normal_mapped`
  only wraps reflective materials (lambertian, metal), not glass. `principled` bends its own normals
  (`set_normal_map`, and the coat's in `set_clearcoat`); wrapping it would bend the coat with the base.
- The clear coat drops the light it reflects back down to the base (glTF's (1 - w F)(1 - w F) model):
  a full coat over a white metal reflects ~92%, so coated materials are only "bounded" in the furnace test.
  Mitsuba's native principled coat is Disney's (GTR1, x1/4, base not attenuated): only `clearcoat-model` is compared.
- Measured metals (`find_conductor`) are RGB (eta, k) reduced from spectral data the way Mitsuba's scalar_rgb does
  (titanium from Johnson & Christy, which Mitsuba lacks). Their Kulla-Conty tint uses F_avg integrated from the
  exact curve. On `principled`, `conductor` replaces Schlick(base color) for the metallic part only.
- `clearcoat_color` is the coat's round-trip transmittance seen head-on; the exponent (1/cos_t,i + 1/cos_t,o)/2 uses
  directions refracted into an ior 1.5 coat, so it never exceeds ~1.34 even at grazing angles. The coat's own
  reflection stays white.
- Glass absorption is applied in `ray_color` through `material::interior_transmittance` on back-face
  hits (the segment ran inside); a segment that ends on something else inside the glass is not tinted.
- A gridded light (`diffuse_light` with `spread` < 180) emits L0 (1 - tan θ / tan(spread/2)): `emitted` depends on the
  direction and `average_emission` is the cosine-weighted mean (so `power()` stays right); `pdf_test` checks the two agree.
  A 100° grid keeps only ~25% of the power, hence the large radiances in `car_studio.scene`.
- `mesh::pdf_value` is linear in triangle count: fine for small emissive meshes only.
- Light selection probabilities are computed once by `light_list::build()` at the end of `load_scene`;
  lights added later are picked uniformly until it is called again. A new emitter type needs `power()`.
- Scene paths (textures, OBJ) are relative to the scene file, not the CWD; MTL and
  its texture paths are relative to the OBJ.
- glTF uvs are flipped on load (v = 1 - v, ours is 0 at the bottom); `uv_transform_texture` flips back to apply
  KHR_texture_transform in glTF's own space. glTF packs roughness in green and metallic in blue (`channel_texture`).
  A material on a glTF `mesh` line replaces all of the file's materials (clay); `default_material` means none given.
  `replace <file material> <scene material>` swaps one by name (e.g. real `thin_glass` for a model's alpha-blended
  window shells); a replaced material drops the file's alpha coverage, and an unknown name is an error.
- glTF emission with a texture becomes `emissive_surface`, which is not in `lights`; only untextured emission is an
  area light, so a large textured emitter (CarConcept's rims carry the logo texture) costs nothing in `sample_light`.
- glTF `BLEND`/`MASK` alpha without a texture is a constant coverage (a 1x1 `image_texture` mask): real-time
  models use see-through shells (the Porsche's 46% "coat") that a path tracer hits with that probability.
- Alpha-masked triangles call `random_double()` inside `hit()` (stochastic coverage);
  `hit()` must not write `rec` before that test, or a rejected hit corrupts the BVH's
  closest-so-far record.
