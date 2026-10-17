#ifndef VOXEL_H
#define VOXEL_H

#include "bvh.h"
#include "hittable_list.h"
#include "material.h"
#include "quad.h"
#include "vox_loader.h"

#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// How a palette index takes part in the surfaces of a voxel model. A surface
// lies wherever two neighbouring cells differ in kind: an opaque cell shows
// its faces towards empty and transparent cells, and a transparent (glass)
// cell its faces towards empty cells only, so a block of glass voxels is one
// volume of glass rather than a stack of separate cubes.
enum class voxel_kind : uint8_t { empty, opaque, transparent };

// The materials of a .vox file's 256 palette entries, in mypbr's terms.
class voxel_palette{
    public:
        std::array<std::shared_ptr<material>, 256> materials;
        std::array<voxel_kind, 256> kinds;
        std::array<bool, 256> emissive;

        voxel_kind kind(uint8_t index) const { return kinds[index]; }
};

// Builds mypbr materials from the palette colors and MATL properties.
//
// MagicaVoxel stores colors sRGB encoded; they are decoded to linear albedo.
// MATL types are mapped to the closest material here (the property names
// come from files MagicaVoxel writes; the format text only lists a few):
//
//   _diffuse          lambertian
//   _metal            principled with metallic _metal (default 1), roughness
//                     _rough, ior _ri (or 1 + _ior); _plastic 1 = metallic 0
//   _glass            dielectric with ior and roughness as above, tinted by
//                     the palette color: light crossing one voxel keeps
//                     mix(white, color, _att) of itself (default _att 0.5)
//   _emit             area light of radiance color * _emit * 10^_flux,
//                     times emission_scale
//   others (_blend, _media, _cloud) fall back to lambertian with a warning
inline voxel_palette make_voxel_palette(const vox_file& vox, double emission_scale) {
    voxel_palette pal;
    bool warned = false;
    auto number = [](const std::map<std::string, std::string>& props, const char* key, double fallback) {
        auto it = props.find(key);
        if (it == props.end())
            return fallback;
        try { return std::stod(it->second); } catch (...) { return fallback; }
    };

    for (int k = 0; k < 256; k++) {
        const color& c = vox.palette[k];
        color albedo(srgb_to_linear(c.x()), srgb_to_linear(c.y()), srgb_to_linear(c.z()));
        const auto& props = vox.materials[k];
        auto type_it = props.find("_type");
        std::string type = type_it == props.end() ? "_diffuse" : type_it->second;

        double roughness = std::clamp(number(props, "_rough", 0.1), 0.0, 1.0);
        double ior = number(props, "_ri", 1 + number(props, "_ior", 0.5));
        if (ior <= 1)
            ior = 1.5;

        pal.kinds[k] = k == 0 ? voxel_kind::empty : voxel_kind::opaque;
        pal.emissive[k] = false;
        if (type == "_metal") {
            double metallic = number(props, "_plastic", 0) > 0 ? 0 : std::clamp(number(props, "_metal", 1), 0.0, 1.0);
            pal.materials[k] = std::make_shared<principled>(std::make_shared<solid_color>(albedo), metallic, roughness, ior);
        } else if (type == "_glass") {
            double att = std::clamp(number(props, "_att", 0.5), 0.0, 1.0);
            color tint = (1 - att) * color(1, 1, 1) + att * albedo;
            for (int i = 0; i < 3; i++)
                tint[i] = std::clamp(tint[i], 1e-3, 1.0);
            pal.materials[k] = std::make_shared<dielectric>(ior, roughness <= 0.05 ? 0 : roughness,
                                                            dielectric::absorption_for(tint, 1));
            pal.kinds[k] = voxel_kind::transparent;
        } else if (type == "_emit") {
            double strength = number(props, "_emit", 1) * std::pow(10.0, number(props, "_flux", 0)) * emission_scale;
            pal.materials[k] = std::make_shared<diffuse_light>(strength * albedo);
            pal.emissive[k] = strength > 0;
        } else {
            if (type != "_diffuse" && !warned && k > 0) {
                std::clog << "Warning: .vox material type " << type << " is rendered as diffuse\n";
                warned = true;
            }
            pal.materials[k] = std::make_shared<lambertian>(albedo);
        }
    }
    return pal;
}

// True if the face of a cell of kind k towards a neighbour of kind n is a
// surface (see voxel_kind).
inline bool voxel_face_visible(voxel_kind k, voxel_kind n) {
    if (k == voxel_kind::empty)
        return false;
    if (n == voxel_kind::empty)
        return true;
    return k == voxel_kind::opaque && n == voxel_kind::transparent;
}

// The visible faces of a model as quads in model coordinates, each with its
// cell's material: the straightforward way to put voxels in the BVH, two
// triangles' worth of geometry per visible face. Faces of emissive cells also
// go to emitters (one list per model, to be sampled as a light).
inline void voxel_model_faces(const vox_model& m, const voxel_palette& pal,
                              hittable_list& faces, hittable_list& emitters) {
    // For each of the six directions: the neighbour offset, and the corner
    // and edges of the face on that side of cell (0, 0, 0), wound so that
    // cross(u, v) points out of the cell
    static const int offsets[6][3] = {{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}};
    static const double corners[6][3] = {{1,0,0}, {0,0,0}, {0,1,0}, {0,0,0}, {0,0,1}, {0,0,0}};
    static const double us[6][3] = {{0,1,0}, {0,0,1}, {0,0,1}, {1,0,0}, {1,0,0}, {0,1,0}};
    static const double vs[6][3] = {{0,0,1}, {0,1,0}, {1,0,0}, {0,0,1}, {0,1,0}, {1,0,0}};

    for (int z = 0; z < m.nz; z++)
        for (int y = 0; y < m.ny; y++)
            for (int x = 0; x < m.nx; x++) {
                uint8_t index = m.at(x, y, z);
                voxel_kind k = pal.kind(index);
                if (k == voxel_kind::empty)
                    continue;
                for (int f = 0; f < 6; f++) {
                    voxel_kind n = pal.kind(m.at(x + offsets[f][0], y + offsets[f][1], z + offsets[f][2]));
                    if (!voxel_face_visible(k, n))
                        continue;
                    point3 Q(x + corners[f][0], y + corners[f][1], z + corners[f][2]);
                    vec3 u(us[f][0], us[f][1], us[f][2]), v(vs[f][0], vs[f][1], vs[f][2]);
                    auto face = std::make_shared<quad>(Q, u, v, pal.materials[index]);
                    faces.add(face);
                    if (pal.emissive[index])
                        emitters.add(face);
                }
            }
}

#endif
