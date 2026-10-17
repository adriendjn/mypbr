# Validating mypbr against Mitsuba 3

Two unbiased renderers given the same scene must converge to the same image;
only their noise differs. `validate.py` renders a set of small test scenes with
mypbr and with [Mitsuba 3](https://www.mitsuba-renderer.org/) and reports where
they disagree by more than noise can explain.

```sh
pip install mitsuba flip-evaluator numpy pillow
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
python3 tools/mitsuba/validate.py              # all cases, about 10 minutes on 4 cores
python3 tools/mitsuba/validate.py glass hdri   # some of them
python3 tools/mitsuba/validate.py --spp 64     # quick and noisy
```

Results go to `tools/mitsuba/out/` (gitignored): per scene the mypbr render
(`.pfm`), and per case the Mitsuba render (`mitsuba.exr`), the generated
`scene.xml` and `compare.png` (mypbr | Mitsuba | relative difference, red where
mypbr is brighter | FLIP error), plus `report.md` with all of them. `--reuse`
keeps renders already there, to re-run only the comparison; delete a `.pfm` to
re-render that scene after changing mypbr. The exit code is 1 when a case does
not give its expected result.

## Files

| File | What it does |
|---|---|
| `cases/*.scene` | The test scenes, in mypbr's format (the single source of truth). |
| `scene_to_mitsuba.py` | Translates a `.scene` to Mitsuba XML: camera, spheres, quads, boxes, transforms, backgrounds, HDR environments, and the lambertian / metal / dielectric / principled / emissive materials. Anything else is an error, not an approximation. |
| `mypbr_bsdf.py` | mypbr's principled/metal BRDF (GGX + Schlick + Kulla-Conty + Lambertian base) re-written as a Mitsuba plugin, `mypbr_principled`. |
| `validate.py` | Renders both, compares, writes the report. |

## How a comparison is judged

- **ratio**: total mypbr radiance over total Mitsuba radiance, per channel. A
  missing or extra few percent of energy shows here even when every pixel is
  noisy.
- **bad blocks**: 16 x 16 pixel blocks whose mean differs by more than 3% *and*
  by more than 4 standard errors. Local errors (a highlight, a rim, a shadow)
  that the total averages away.
- **rel. RMSE** and **FLIP** (NVIDIA's perceptual error) mostly measure noise;
  they are there to compare runs.

A case matches when the total is within 1% (or the difference is not
significant) and no block is bad.

## Native plugins or the same model

For most materials there is no choice to make: a Lambertian is a Lambertian,
smooth glass is Fresnel plus Snell, rough glass is Walter et al.'s GGX BSDF.
Those always use Mitsuba's own plugins, so they are checked against a
completely independent implementation.

mypbr's metal and principled are *models* with choices in them (Schlick
Fresnel, height-correlated Smith G, Kulla-Conty energy compensation, a
Lambertian base scaled by (1 - F)(1 - F)), and no Mitsuba plugin makes the same
choices. So those cases run twice:

- `*-model`: Mitsuba with `mypbr_principled`, the same formulas written again
  in Dr.Jit (with its own E(mu) table, by quadrature instead of VNDF
  sampling). This checks mypbr's implementation: eval, pdf, sampling, MIS.
  Should match.
- `*-native`: Mitsuba's `principled`. Shows how much the model choices matter.
  Expected to differ.
- `metal-single-scatter`: `mypbr_principled` without the Kulla-Conty lobe, to
  show the energy the compensation adds.

## Results (October 2026, Mitsuba 3.9.1)

| case | result |
|---|---|
| cornell | matches (total within 0.01%) |
| metal-model | matches |
| metal-single-scatter | mypbr brighter by 2.5 to 8% overall, up to 2x on the sides of the roughest spheres: the Kulla-Conty energy, as intended |
| metal-native | the same plus 0.3%: height-correlated vs separable Smith G barely matters |
| principled-model | matches |
| principled-native | mypbr 2 to 3% darker overall, up to 23% at the rims: its base only gets the light the specular layer lets through, (1 - F)(1 - F), while Mitsuba's Disney diffuse brightens rough surfaces at grazing angles |
| glass | matches, after switching smooth glass from Schlick to exact Fresnel (the rims were up to 10% too dark) |
| colored-glass | matches |
| hdri | matches: environment orientation, rotation and importance sampling |

## Gotchas

- Mitsuba's `max_depth` counts path segments, mypbr's counts surface hits:
  the translation adds one.
- Mitsuba's reflective BSDFs are one-sided, mypbr's are not (the normal is
  flipped towards the ray): they are wrapped in `twosided`.
- The environment map needs a 180 degree turn about y (plus mypbr's
  `rotate`): mypbr puts the middle of the image at -z, Mitsuba the left edge.
- Mitsuba's default film filter is a Gaussian; the translation asks for a box,
  like mypbr's.
- Colored glass becomes a dielectric around a homogeneous medium with albedo 0
  (pure absorption) and needs the `volpath` integrator. Keep other objects out
  of the glass: mypbr does not tint a segment that ends on something inside it,
  Mitsuba does.
