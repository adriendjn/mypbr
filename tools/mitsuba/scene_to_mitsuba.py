#!/usr/bin/env python3
"""Translates a mypbr .scene file into a Mitsuba 3 XML scene.

Only the subset the validation cases need is supported: camera (pinhole),
spheres, quads, boxes, transforms, constant backgrounds, HDR environments and
the lambertian / metal / dielectric / principled / emissive materials. Anything
else (textures, meshes, media, depth of field) is an error rather than a silent
approximation, so a comparison never runs on a scene that differs.

Each mypbr material can be translated two ways (--bsdfs):

  native  Mitsuba's own plugins, the closest physical equivalent. Differences
          against mypbr then mix implementation bugs with model choices
          (Smith G, Fresnel approximation, energy compensation).
  mypbr   Rough metal and principled use the mypbr_principled plugin
          (mypbr_bsdf.py), the same formulas as include/material.h written
          independently in Dr.Jit. Any difference is then an implementation
          bug in one of the two, or noise. Lambertian, mirrors and glass have
          no model choice to make and always use native plugins.
  mypbr-ss  As mypbr, without the Kulla-Conty lobe (single-scattering GGX):
          the difference to mypbr is the energy the compensation adds back.

Usage: scene_to_mitsuba.py scene.scene [--bsdfs native|mypbr|mypbr-ss] [-o out.xml]
"""

import argparse
import math
import os
import sys

import numpy as np


class SceneError(Exception):
    pass


# --- Transforms (4x4 matrices, same conventions as include/transform.h) ---

def translate(d):
    m = np.eye(4)
    m[:3, 3] = d
    return m


def scale(f):
    return np.diag([f[0], f[1], f[2], 1.0])


def rotate(degrees, axis):
    # Rodrigues' formula, as transform::rotate
    a = np.asarray(axis, float) / np.linalg.norm(axis)
    t = math.radians(degrees)
    c, s, C = math.cos(t), math.sin(t), 1 - math.cos(t)
    x, y, z = a
    m = np.eye(4)
    m[:3, :3] = [[x*x*C + c,   x*y*C - z*s, x*z*C + y*s],
                 [y*x*C + z*s, y*y*C + c,   y*z*C - x*s],
                 [z*x*C - y*s, z*y*C + x*s, z*z*C + c]]
    return m


def parse_transforms(tokens):
    """Reads "translate x y z | scale s | scale x y z | rotate_[xyz] deg | rotate deg ax ay az"
    until tokens run out. Applied in the order given, so the result is T_n ... T_1."""
    m = np.eye(4)
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t == "translate":
            step = translate([float(v) for v in tokens[i+1:i+4]])
            i += 4
        elif t in ("rotate_x", "rotate_y", "rotate_z"):
            axis = [t == "rotate_x", t == "rotate_y", t == "rotate_z"]
            step = rotate(float(tokens[i+1]), axis)
            i += 2
        elif t == "rotate":
            step = rotate(float(tokens[i+1]), [float(v) for v in tokens[i+2:i+5]])
            i += 5
        elif t == "scale":
            vals = []
            j = i + 1
            while j < len(tokens) and len(vals) < 3 and _is_number(tokens[j]):
                vals.append(float(tokens[j]))
                j += 1
            if len(vals) not in (1, 3):
                raise SceneError("scale takes 1 or 3 values")
            step = scale(vals * 3 if len(vals) == 1 else vals)
            i = j
        else:
            raise SceneError(f"unexpected token '{t}'")
        m = step @ m
    return m


def _is_number(s):
    try:
        float(s)
        return True
    except ValueError:
        return False


def fmt_matrix(m):
    return " ".join(f"{v:.9g}" for v in m.flatten())


def fmt_rgb(c):
    return ", ".join(f"{v:.9g}" for v in c)


# --- Materials ---

def roughness_to_alpha(r):
    # microfacet.h: alpha = roughness^2, at least 1e-3
    return max(r * r, 1e-3)


class Material:
    """A parsed mypbr material and its Mitsuba translation."""

    def __init__(self, name, tokens):
        self.name = name
        self.kind = tokens[0]
        self.emission = None
        self.absorption = None  # sigma_a of a colored dielectric
        args = tokens[1:]
        if "texture" in args or "normal_map" in args:
            raise SceneError(f"material '{name}': textures and normal maps are not supported")
        if self.kind == "lambertian":
            self.albedo = [float(v) for v in args[:3]]
        elif self.kind == "metal":
            self.albedo = [float(v) for v in args[:3]]
            self.roughness = min(max(float(args[3]), 0.0), 1.0)
        elif self.kind == "dielectric":
            self.ior = float(args[0])
            self.roughness = 0.0
            tint, distance = None, 1.0
            i = 1
            while i < len(args):
                if args[i] == "roughness":
                    self.roughness = float(args[i+1]); i += 2
                elif args[i] == "color":
                    tint = [float(v) for v in args[i+1:i+4]]; i += 4
                elif args[i] == "distance":
                    distance = float(args[i+1]); i += 2
                else:
                    raise SceneError(f"material '{name}': unexpected '{args[i]}'")
            if tint is not None:
                # dielectric::absorption_for: exp(-sigma_a d) = tint
                self.absorption = [-math.log(c) / distance for c in tint]
        elif self.kind == "principled":
            self.albedo, self.metallic, self.roughness, self.ior = [0.8] * 3, 0.0, 0.5, 1.5
            i = 0
            while i < len(args):
                key = args[i]
                if key == "color":
                    self.albedo = [float(v) for v in args[i+1:i+4]]; i += 4
                elif key in ("metallic", "roughness", "ior"):
                    setattr(self, key, float(args[i+1])); i += 2
                else:
                    raise SceneError(f"material '{name}': unexpected '{key}'")
        elif self.kind == "emissive":
            self.emission = [float(v) for v in args[:3]]
        else:
            raise SceneError(f"material '{name}': type '{self.kind}' is not supported")

    def medium_xml(self):
        """A homogeneous, purely absorbing interior for colored glass (Beer-Lambert)."""
        if self.absorption is None:
            return ""
        return (f'    <medium type="homogeneous" id="{self.name}_interior">\n'
                f'        <rgb name="albedo" value="0, 0, 0"/>\n'
                f'        <rgb name="sigma_t" value="{fmt_rgb(self.absorption)}"/>\n'
                f'    </medium>\n')

    def bsdf_xml(self, bsdfs):
        """The <bsdf> for this material. mypbr shades both sides of reflective
        surfaces (the normal is flipped towards the ray), Mitsuba's reflective
        BSDFs are one-sided, hence the twosided wrappers."""
        if self.kind == "lambertian":
            inner = (f'<bsdf type="diffuse"><rgb name="reflectance" value="{fmt_rgb(self.albedo)}"/></bsdf>')
            return f'<bsdf type="twosided" id="{self.name}">{inner}</bsdf>'
        if self.kind == "emissive":
            # diffuse_light scatters nothing
            return f'<bsdf type="diffuse" id="{self.name}"><rgb name="reflectance" value="0, 0, 0"/></bsdf>'
        if self.kind == "metal" and self.roughness == 0:
            # A mirror reflects albedo at every angle (no Fresnel): Mitsuba's
            # "none" conductor is a perfect mirror, scaled by specular_reflectance
            inner = ('<bsdf type="conductor"><string name="material" value="none"/>'
                     f'<rgb name="specular_reflectance" value="{fmt_rgb(self.albedo)}"/></bsdf>')
            return f'<bsdf type="twosided" id="{self.name}">{inner}</bsdf>'
        if self.kind == "dielectric":
            if self.roughness == 0:
                return (f'<bsdf type="dielectric" id="{self.name}">'
                        f'<float name="int_ior" value="{self.ior}"/><float name="ext_ior" value="1"/></bsdf>')
            return (f'<bsdf type="roughdielectric" id="{self.name}">'
                    f'<string name="distribution" value="ggx"/>'
                    f'<float name="alpha" value="{roughness_to_alpha(self.roughness):.9g}"/>'
                    f'<float name="int_ior" value="{self.ior}"/><float name="ext_ior" value="1"/></bsdf>')
        # Rough metal and principled
        metallic = 1.0 if self.kind == "metal" else self.metallic
        ior = 1.5 if self.kind == "metal" else self.ior
        if bsdfs in ("mypbr", "mypbr-ss"):
            inner = (f'<bsdf type="mypbr_principled">'
                     f'<boolean name="compensation" value="{str(bsdfs == "mypbr").lower()}"/>'
                     f'<rgb name="base_color" value="{fmt_rgb(self.albedo)}"/>'
                     f'<float name="metallic" value="{metallic}"/>'
                     f'<float name="roughness" value="{self.roughness}"/>'
                     f'<float name="ior" value="{ior}"/></bsdf>')
        else:
            # Mitsuba's principled: same GGX alpha = roughness^2 and, when
            # metallic, the same Schlick Fresnel on base_color; but separable
            # Smith G, no multiple-scattering compensation, and a Disney
            # (Burley) diffuse instead of a Lambertian base.
            inner = (f'<bsdf type="principled">'
                     f'<rgb name="base_color" value="{fmt_rgb(self.albedo)}"/>'
                     f'<float name="metallic" value="{metallic}"/>'
                     f'<float name="roughness" value="{self.roughness}"/>'
                     f'<float name="eta" value="{ior}"/></bsdf>')
        return f'<bsdf type="twosided" id="{self.name}">{inner}</bsdf>'


# --- Scene ---

class Scene:
    def __init__(self):
        self.width = 400
        self.aspect = 16 / 9
        self.spp = 10
        self.max_depth = 10
        self.lookfrom, self.lookat, self.vup, self.vfov = [0, 0, 0], [0, 0, -1], [0, 1, 0], 90.0
        self.background = None   # rgb
        self.environment = None  # (path, intensity, rotation)
        self.materials = {}
        self.shapes = []         # xml snippets

    @property
    def height(self):
        return max(int(self.width / self.aspect), 1)

    @property
    def needs_volumes(self):
        return any(m.absorption is not None for m in self.materials.values())


def load(path):
    sc = Scene()
    base = os.path.dirname(os.path.abspath(path))
    with open(path) as f:
        lines = f.readlines()
    for number, line in enumerate(lines, 1):
        tokens = line.split("#", 1)[0].split()
        if not tokens:
            continue
        try:
            _parse_line(sc, base, tokens)
        except (SceneError, ValueError, IndexError) as e:
            raise SceneError(f"{path}:{number}: {e}") from None
    if sc.background is None and sc.environment is None:
        raise SceneError(f"{path}: needs 'background' or 'environment' (the default sky gradient has no Mitsuba equivalent)")
    return sc


def _parse_line(sc, base, tokens):
    key, args = tokens[0], tokens[1:]
    if key == "image_width":
        sc.width = int(args[0])
    elif key == "aspect_ratio":
        num, _, den = args[0].partition("/")
        sc.aspect = float(num) / float(den) if den else float(num)
    elif key == "samples_per_pixel":
        sc.spp = int(args[0])
    elif key == "max_depth":
        sc.max_depth = int(args[0])
    elif key == "camera":
        i = 0
        while i < len(args):
            k = args[i]
            if k in ("lookfrom", "lookat", "vup"):
                setattr(sc, k, [float(v) for v in args[i+1:i+4]]); i += 4
            elif k == "vfov":
                sc.vfov = float(args[i+1]); i += 2
            elif k == "aperture":
                if float(args[i+1]) != 0:
                    raise SceneError("depth of field is not supported")
                i += 2
            elif k == "focus":
                i += 2
            else:
                raise SceneError(f"unexpected camera setting '{k}'")
    elif key == "aperture":
        if float(args[0]) != 0:
            raise SceneError("depth of field is not supported")
    elif key in ("focus", "exposure", "tonemap", "light_selection", "normal_maps"):
        pass  # Display or sampling settings: they don't change the radiance
    elif key == "background":
        sc.background = [float(v) for v in args[:3]]
    elif key == "environment":
        intensity, rotation = 1.0, 0.0
        i = 1
        while i < len(args):
            if args[i] == "intensity":
                intensity = float(args[i+1])
            elif args[i] == "rotate":
                rotation = float(args[i+1])
            else:
                raise SceneError(f"unexpected '{args[i]}'")
            i += 2
        sc.environment = (os.path.normpath(os.path.join(base, args[0])), intensity, rotation)
    elif key == "material":
        sc.materials[args[0]] = Material(args[0], args[1:])
    elif key in ("sphere", "quad", "box"):
        n = {"sphere": 4, "quad": 9, "box": 6}[key]
        values = [float(v) for v in args[:n]]
        rest = args[n:]
        mat = None
        if rest and rest[0] in sc.materials:
            mat = sc.materials[rest[0]]
            rest = rest[1:]
        elif rest and not rest[0].startswith(("translate", "scale", "rotate")):
            raise SceneError(f"unknown material '{rest[0]}'")
        xform = parse_transforms(rest)
        if mat is None:
            if key != "sphere":
                raise SceneError(f"{key} needs a material")
            mat = sc.materials.setdefault("_default", Material("_default", ["lambertian", "0.5", "0.5", "0.5"]))
        if key == "sphere":
            c, r = np.array(values[:3]), values[3]
            m = xform @ translate(c) @ scale([r, r, r])
            sc.shapes.append(_shape_xml("sphere", m, mat))
        elif key == "quad":
            q, u, v = (np.array(values[k:k+3]) for k in (0, 3, 6))
            sc.shapes.append(_quad_xml(q, u, v, xform, mat))
        else:
            a, b = np.array(values[:3]), np.array(values[3:])
            lo, hi = np.minimum(a, b), np.maximum(a, b)
            dx, dy, dz = (np.eye(3)[k] * (hi - lo)[k] for k in range(3))
            # The six faces of box() in quad.h, normals outwards
            faces = [((lo[0], lo[1], hi[2]),  dx,  dy),
                     ((hi[0], lo[1], hi[2]), -dz,  dy),
                     ((hi[0], lo[1], lo[2]), -dx,  dy),
                     ((lo[0], lo[1], lo[2]),  dz,  dy),
                     ((lo[0], hi[1], hi[2]),  dx, -dz),
                     ((lo[0], lo[1], lo[2]),  dx,  dz)]
            for q, u, v in faces:
                sc.shapes.append(_quad_xml(np.array(q), u, v, xform, mat))
    else:
        raise SceneError(f"'{key}' is not supported")


def _quad_xml(q, u, v, xform, mat):
    # Mitsuba's rectangle spans [-1, 1]^2 in the xy plane with normal +z, so
    # map x -> u/2, y -> v/2, z -> the quad's normal, origin -> its center.
    # The normal (transformed by the inverse transpose) comes out along
    # cross(u, v), mypbr's front face.
    n = np.cross(u, v)
    n /= np.linalg.norm(n)
    m = np.eye(4)
    m[:3, 0], m[:3, 1], m[:3, 2], m[:3, 3] = u / 2, v / 2, n, q + u / 2 + v / 2
    return _shape_xml("rectangle", xform @ m, mat)


def _shape_xml(kind, m, mat):
    s = f'    <shape type="{kind}">\n'
    s += f'        <transform name="to_world"><matrix value="{fmt_matrix(m)}"/></transform>\n'
    s += f'        <ref id="{mat.name}"/>\n'
    if mat.absorption is not None:
        s += f'        <ref name="interior" id="{mat.name}_interior"/>\n'
    if mat.emission is not None:
        s += f'        <emitter type="area"><rgb name="radiance" value="{fmt_rgb(mat.emission)}"/></emitter>\n'
    s += '    </shape>\n'
    return s


def to_xml(sc, bsdfs="native", spp=None):
    """The Mitsuba 3 scene as an XML string. spp defaults to the scene's."""
    integrator = "volpath" if sc.needs_volumes else "path"
    # mypbr's max_depth counts surface hits; Mitsuba's counts path segments,
    # one more (the segment to the light found by the last bounce's NEE).
    x = '<scene version="3.0.0">\n'
    x += (f'    <integrator type="{integrator}">'
          f'<integer name="max_depth" value="{sc.max_depth + 1}"/></integrator>\n')
    x += ('    <sensor type="perspective">\n'
          f'        <string name="fov_axis" value="y"/><float name="fov" value="{sc.vfov}"/>\n'
          '        <transform name="to_world">'
          f'<lookat origin="{fmt_rgb(sc.lookfrom)}" target="{fmt_rgb(sc.lookat)}" up="{fmt_rgb(sc.vup)}"/>'
          '</transform>\n'
          f'        <sampler type="independent"><integer name="sample_count" value="{spp or sc.spp}"/></sampler>\n'
          f'        <film type="hdrfilm"><integer name="width" value="{sc.width}"/>'
          f'<integer name="height" value="{sc.height}"/>'
          '<string name="pixel_format" value="rgb"/><rfilter type="box"/></film>\n'
          '    </sensor>\n')
    if sc.environment:
        path, intensity, rotation = sc.environment
        # environment.h maps image column u to the direction at azimuth
        # 2 pi (u - 0.5 - rotation) (x = sin, -z = cos); Mitsuba's envmap puts
        # u at 2 pi u in its local frame. Turning the map about y by
        # 180 degrees + rotation lines the two up.
        x += ('    <emitter type="envmap">'
              f'<string name="filename" value="{path}"/><float name="scale" value="{intensity}"/>'
              f'<transform name="to_world"><rotate y="1" angle="{180 + rotation}"/></transform>'
              '</emitter>\n')
    elif sc.background and any(sc.background):
        x += f'    <emitter type="constant"><rgb name="radiance" value="{fmt_rgb(sc.background)}"/></emitter>\n'
    for mat in sc.materials.values():
        x += mat.medium_xml()
        x += "    " + mat.bsdf_xml(bsdfs) + "\n"
    x += "".join(sc.shapes)
    x += "</scene>\n"
    return x


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scene")
    parser.add_argument("--bsdfs", choices=["native", "mypbr", "mypbr-ss"], default="native")
    parser.add_argument("-o", "--output", help="XML file to write (default: stdout)")
    args = parser.parse_args()
    try:
        xml = to_xml(load(args.scene), args.bsdfs)
    except SceneError as e:
        sys.exit(f"error: {e}")
    if args.output:
        with open(args.output, "w") as f:
            f.write(xml)
    else:
        sys.stdout.write(xml)


if __name__ == "__main__":
    main()
