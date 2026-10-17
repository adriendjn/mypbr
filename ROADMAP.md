# mypbr roadmap

Where the renderer is today: milestones 1 to 13 are done (renders in `renders/NN-*.png`). It is a multithreaded CPU path tracer with NEE + MIS, GGX microfacets with energy compensation, a SAH BVH, OBJ/MTL meshes, HDRI lighting, fog, normal maps and an à-trous denoiser. `CLAUDE.md` describes the code.

The plan started from Ray Tracing in One Weekend chapter 6 (spheres, a `camera` class, normal shading, PPM to stdout).

Each milestone below ends with a render you can look at. Do them in order; every one builds on the previous.

---

## 1. Antialiasing and an RNG

- Add `random_double()` / `random_double(min, max)` to `rtweekend.h`. Use one `std::mt19937` per thread (`thread_local`) so milestone 9 is painless.
- `camera::samples_per_pixel`; jitter each sample inside the pixel square (`get_ray(i, j)`).
- Accumulate, then divide by the sample count in `write_color`, clamped with `interval(0.000, 0.999)`.
- Scene file: `samples_per_pixel <n>`.

**Done when:** sphere edges are smooth at 100 spp.

## 2. Diffuse bounces and gamma

- `random_unit_vector()` (rejection sampling in the unit sphere, then normalize) and `random_on_hemisphere()`.
- Recursive `ray_color` with `camera::max_depth` (start at 10 to 50).
- Fix shadow acne: hit interval starts at `0.001`, not `0`.
- Lambertian scatter: `normal + random_unit_vector()` (this is already cosine-weighted, which matters in milestone 6).
- `linear_to_gamma` (sqrt for gamma 2) in `write_color`.
- Add a ground sphere to `default.scene`.
- Scene file: `max_depth <n>`.

**Done when:** a grey sphere on a grey ground shows soft contact shadows.

## 3. Materials

- `material` base class with `scatter(r_in, rec, attenuation, scattered)`; `hit_record` gets a `shared_ptr<material>`.
- `lambertian(albedo)`, `metal(albedo, fuzz)`, `dielectric(ior)` with Snell, total internal reflection and Schlick.
- Scene file (the planned extension):
  ```
  material ground lambertian 0.8 0.8 0.0
  material glass  dielectric 1.5
  material gold   metal 0.8 0.6 0.2 0.3
  sphere 0 -100.5 -1  100  ground
  sphere 0 0 -1  0.5  glass
  ```
  Unknown material name is a parse error with the line number. A sphere with no name gets a default Lambertian.
- Try a hollow glass sphere (negative radius inner sphere) as a test.

**Done when:** the book's three-sphere scene renders and matches the reference look.

## 4. Positionable camera

- `vfov`, `lookfrom`, `lookat`, `vup`; build the `u, v, w` basis.
- Defocus blur: `defocus_angle`, `focus_dist`, sample on a disk.
- Scene file: `camera lookfrom x y z lookat x y z vup x y z vfov deg`, plus `aperture`/`focus`.
- Optional: `-o out.ppm` or PNG output via `stb_image_write.h` so you stop piping stdout.

**Done when:** the "Weekend" final cover scene renders (small, low spp is fine).

## 5. Lights and a Cornell box

- `material::emitted()` (returns black by default), `diffuse_light(color)`.
- `ray_color` = emitted + attenuation * incoming. Configurable background color; set it to black for interior scenes.
- New primitive: `quad` (corner + two edge vectors). Use it for walls and the light.
- `box()` helper returning six quads.
- Scene file: `background r g b`, `quad Q u v material`, `material light emissive r g b`.

**Done when:** a Cornell box renders. It will be very noisy; that is the motivation for milestone 6.

## 6. Make it actually physically based

This is where it stops being "a ray tracer" and becomes a PBR path tracer.

- Rewrite the integrator as an estimator: `f * cos / pdf`. Give each material `scattering_pdf` and the sampler its `pdf`.
- `pdf` classes: `cosine_pdf` (orthonormal basis around the normal), `hittable_pdf` (sample a point on a light), `mixture_pdf`.
- Light sampling (next event estimation): at each diffuse hit, shoot a shadow ray to a light. Combine with BSDF sampling using **multiple importance sampling** (power heuristic) so small lights and glossy surfaces both converge.
- Specular materials (metal, glass) skip the PDF path; keep a `skip_pdf` flag.
- Russian roulette after ~3 bounces instead of a hard depth cut.
- Stratified (jittered grid) pixel samples.
- Sanity checks: a white furnace test (all-white diffuse sphere in a uniform white environment must render uniform white), and guard against NaN/inf samples.

**Done when:** the Cornell box at the same spp is dramatically cleaner than milestone 5, and the furnace test passes.

## 7. HDR output and tonemapping

- Keep a `std::vector<color>` float framebuffer instead of writing pixels as you go.
- Write `.pfm` or `.hdr` (raw radiance), and an 8-bit PNG after exposure + tonemap (Reinhard, then ACES fitted).
- Proper sRGB transfer curve instead of plain sqrt.
- Progressive rendering: add passes and save every N, so long renders can be stopped early.

**Done when:** a bright light in frame no longer clips the whole room to white.

## 8. Better materials

- Microfacet BRDF: GGX distribution, Smith masking, Schlick Fresnel; roughness parameter for `metal`.
- A single `principled` / "standard surface" material (base color, metallic, roughness, IOR) with a diffuse + specular layer.
- Rough dielectric (GGX transmission) once the reflective version works.
- Textures: `texture` base, solid color, checker, image textures via `stb_image.h` (needs UVs on spheres and quads).

**Done when:** a row of spheres with roughness 0 to 1 looks plausible, and energy stays bounded in the furnace test.

## 9. Speed

Do this when renders start taking minutes, probably around milestone 6.

- **Multithreading:** split the image into tiles, a `std::atomic<int>` tile counter, `std::thread` workers. Per-thread RNG from milestone 1. Expect close to linear speedup.
- **BVH:** axis-aligned bounding boxes on every hittable, then a BVH built with the surface area heuristic (start with median split, upgrade to SAH). Flatten to an array for traversal.
- Profile first (`perf`, or `-pg`) before micro-optimising. Check `-O3 -march=native` in the Release config.
- Later: SIMD-friendly `vec3` or packet traversal, if you want to go deep.

**Done when:** a 1000-sphere scene renders in seconds, not minutes.

## 10. Real meshes

- `triangle` primitive (Möller-Trumbore), with per-vertex normals and UVs.
- OBJ loader (tinyobjloader or a small hand-written one) plus `mesh path material` in the scene file; transforms (translate, rotate, scale) via instancing.
- BVH over triangles is mandatory here.
- Test scenes: Stanford bunny, Sponza, the original Cornell box geometry.

**Done when:** the Stanford bunny renders in glass inside the Cornell box.

## 11. Beyond (done 2026-10-17)

- ✓ Environment lighting from an HDRI with importance sampling (2D CDF over the equirectangular map). PR #2.
- ✓ MTL materials and textures for Sponza, with alpha masks. PR #3.
- ✓ Participating media: constant-density fog with a Henyey-Greenstein phase function. PR #4.
- ✓ A denoiser: our own albedo/normal guided à-trous filter (`--denoise`, `--noisy`) instead of Open Image Denoise. PR #5.

## 12. Materials (done 2026-10-17, PR #6)

- ✓ Tangent-space normal maps (`normal_mapped`, MTL `norm`/`map_Bump`), textured roughness and metallic on `principled`.
- ✓ Colored glass with Beer-Lambert absorption.
- ✓ Kulla-Conty multiple-scattering compensation for rough GGX metal and principled specular.
- ✓ Light selection in proportion to power (20% kept uniform).

Known gaps: grey-scale bump (height) maps are skipped, rough glass is not energy-compensated, the denoiser's normal guide ignores normal maps.

## Validation and showcase (done 2026-10-17)

- ✓ Showcase still life (`scenes/showcase.scene`) using most features at once. PR #8.
- ✓ Mitsuba 3 comparison (`tools/mitsuba/validate.py`): Cornell box, metal, principled, glass, colored glass and HDRI cases match; the differences left are intended (Kulla-Conty energy, mypbr's layered principled base). It found and fixed too-dark smooth-glass rims (Schlick replaced by exact Fresnel). PR #9.

## 13. MagicaVoxel look (done 2026-10-17, PR #10)

From the analysis "Making mypbr look like MagicaVoxel": the look comes from flat-colored cubes, one soft sun plus a sky, a ground fading into the horizon, ACES, bloom and a shallow depth of field.

- ✓ `.vox` loader (models, palette, MATL materials, scene graph), first as quads in the BVH (`vox ... quads`).
- ✓ Sun (`sun`), uniform sky (`sky uniform`), infinite ground (`ground`).
- ✓ Bloom, vignette and polygonal bokeh (`bloom`, `vignette`, `camera blades`).
- ✓ Voxel grid traced with a 3D DDA in 8³ blocks, checked against the quads by `voxel_test`.
- ✓ Physically based sky: single-scattering Rayleigh + Mie + ozone (`sky atmosphere`).
- ✓ Orthographic camera, height fog, MagicaVoxel's bounce caps, transparent shadows, voxel outlines.
- ✓ `style voxel` preset (and `style classic`).

Renders: `renders/13-*.png`. Not done: rOBJ/rCAM import (a .vox file's own render settings and camera), cloud/media voxels, multiple scattering in the sky.

## Next ideas

- Extend the Mitsuba validation to textures, normal maps, fog and meshes.
- Milestone 13 leftovers: rOBJ/rCAM import, cloud/media voxels, multiple scattering in the sky.
- Heterogeneous volumes (grid densities, delta tracking).
- Spectral rendering (wavelengths instead of RGB) for dispersion in glass.
- Bidirectional path tracing for caustics and hard-to-reach lights.
- Close the known gaps above.

---

## Habits worth keeping throughout

- Keep a `scenes/` folder of test scenes and save a reference render per milestone so regressions are obvious.
- Commit at the end of every milestone with the render in the commit message or a `renders/` folder.
- Reading list: *Ray Tracing: The Next Week* (milestones 4, 5, 8, 9), *Ray Tracing: The Rest of Your Life* (milestone 6), and *Physically Based Rendering* (pbrt-v4, free online) for milestones 6 to 11.
