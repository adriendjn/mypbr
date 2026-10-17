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

// A block of a voxel model, walked cell by cell along the ray with a 3D DDA
// (Amanatides and Woo, "A Fast Voxel Traversal Algorithm for Ray Tracing",
// 1987).
//
// Why a grid instead of faces in a BVH: the cells are regular, so from the
// cell a ray is in, the next one is found in O(1) by asking which of the
// three axis planes ahead of it the ray crosses first. The distance to the
// next x plane grows by 1 / |d.x| from one plane to the next (likewise for y
// and z), so stepping is an add and a compare, and the model needs one byte
// per cell instead of a few hundred per visible face.
//
// A walk is only fast while it meets something, though: a ray crossing a
// mostly empty 126^3 model would visit hundreds of empty cells. So a model
// is cut into blocks (voxel_blocks, 8^3 cells), each block shrunk to the
// box of its voxels goes in a BVH, and each walk stays inside its block: the
// BVH skips the empty space, and the walks are short.
//
// A surface is where two neighbouring cells differ in kind (voxel_kind), so
// whether a crossing is a surface depends only on the two cells. A block
// looks its neighbours' cells up in the whole model, so a surface on a block
// boundary is found by the block on either side, at the same t, and the
// result is the same as one walk through the whole model, or as
// voxel_model_faces (tests/voxel_test.cpp checks both).
//
// Model coordinates: cell (x, y, z) covers [x, x+1] x [y, y+1] x [z, z+1].
class voxel_grid : public hittable{
    public:
        // The block of cells [lo, hi) of model.
        voxel_grid(std::shared_ptr<const vox_model> model, std::shared_ptr<const voxel_palette> palette,
                   const int lo[3], const int hi[3])
            : model(model), palette(palette), cells(model->cells.data()),
              nx(model->nx), ny(model->ny), nz(model->nz)
        {
            for (int a = 0; a < 3; a++) {
                this->lo[a] = lo[a];
                this->hi[a] = hi[a];
            }
            for (int k = 0; k < 256; k++) {
                kinds[k] = palette->kinds[k];
                mats[k] = palette->materials[k].get();
            }
            bbox = aabb(point3(lo[0], lo[1], lo[2]), point3(hi[0], hi[1], hi[2]));
        }

        aabb bounding_box() const override { return bbox; }

        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            const point3& o = r.origin();
            const vec3& d = r.direction();

            // Where the ray's line enters and leaves the block
            double t_enter = -infinity, t_exit = infinity;
            int enter_axis = 0;
            for (int a = 0; a < 3; a++) {
                if (d[a] == 0) {
                    if (o[a] < lo[a] || o[a] > hi[a])
                        return false;
                    continue;
                }
                double t0 = (lo[a] - o[a]) / d[a], t1 = (hi[a] - o[a]) / d[a];
                if (t0 > t1)
                    std::swap(t0, t1);
                if (t0 > t_enter) {
                    t_enter = t0;
                    enter_axis = a;
                }
                t_exit = std::fmin(t_exit, t1);
            }
            bool from_outside = t_enter >= ray_t.min;
            double t_start = from_outside ? t_enter : ray_t.min;
            if (t_start > std::fmin(t_exit, ray_t.max))
                return false;

            // The first cell, and the DDA state: for each axis, the step
            // direction, the t of the next plane crossed, and the t between planes
            point3 start = r.at(t_start);
            int c[3], step[3];
            double t_next[3], t_delta[3];
            for (int a = 0; a < 3; a++) {
                c[a] = std::clamp(int(std::floor(start[a])), lo[a], hi[a] - 1);
                if (from_outside && a == enter_axis)  // The layer just inside the entry face
                    c[a] = d[a] > 0 ? lo[a] : hi[a] - 1;
                if (d[a] > 0) {
                    step[a] = 1;
                    t_next[a] = (c[a] + 1 - o[a]) / d[a];
                    t_delta[a] = 1 / d[a];
                } else if (d[a] < 0) {
                    step[a] = -1;
                    t_next[a] = (c[a] - o[a]) / d[a];
                    t_delta[a] = -1 / d[a];
                } else {
                    step[a] = 0;
                    t_next[a] = t_delta[a] = infinity;
                }
            }

            uint8_t index = at(c);
            if (from_outside) {
                // Crossing into the block from the cell behind the entry face
                int a = enter_axis;
                int behind[3] = {c[0], c[1], c[2]};
                behind[a] -= step[a];
                if (crossing(r, t_enter, a, step[a], at(behind), index, rec))
                    return true;
            }

            while (true) {
                // The nearest plane ahead decides the next cell
                int a = t_next[0] < t_next[1] ? (t_next[0] < t_next[2] ? 0 : 2) : (t_next[1] < t_next[2] ? 1 : 2);
                double t = t_next[a];
                if (t > ray_t.max)
                    return false;
                c[a] += step[a];
                t_next[a] += t_delta[a];
                uint8_t next = at(c);  // 0 outside the model
                if (crossing(r, t, a, step[a], index, next, rec))
                    return true;
                if (c[a] < lo[a] || c[a] >= hi[a])
                    return false;  // Left the block
                index = next;
            }
        }

    private:
        std::shared_ptr<const vox_model> model;
        std::shared_ptr<const voxel_palette> palette;  // Owns the materials
        const uint8_t* cells;  // model's, read directly in the inner loop
        int nx, ny, nz;
        int lo[3], hi[3];
        voxel_kind kinds[256];
        const material* mats[256];
        aabb bbox;

        // The palette index of a cell of the whole model, 0 outside it.
        uint8_t at(const int c[3]) const {
            if (unsigned(c[0]) >= unsigned(nx) || unsigned(c[1]) >= unsigned(ny) || unsigned(c[2]) >= unsigned(nz))
                return 0;
            return cells[(size_t(c[2]) * ny + c[1]) * nx + c[0]];
        }

        // The ray crosses, at t, the plane perpendicular to axis from the cell
        // of palette index from to the next one (step is +1 or -1 along axis).
        // Records a hit if that is a surface: the next cell's face, entering
        // it, or else the face of the cell being left, seen from inside (out
        // of glass, or out of a cell the ray started in).
        bool crossing(const ray& r, double t, int axis, int step, uint8_t from, uint8_t to,
                      hit_record& rec) const {
            voxel_kind k_from = kinds[from], k_to = kinds[to];
            if (k_to != voxel_kind::empty && voxel_face_visible(k_to, k_from))
                return record(r, t, axis, -step, to, rec);
            if (voxel_face_visible(k_from, k_to))
                return record(r, t, axis, step, from, rec);
            return false;
        }

        // Fills rec for a hit at t on the face perpendicular to axis whose
        // outward normal (from the cell of palette index) points along sign.
        bool record(const ray& r, double t, int axis, int sign, uint8_t index, hit_record& rec) const {
            rec.t = t;
            rec.p = r.at(t);
            int b = (axis + 1) % 3, c = (axis + 2) % 3;
            vec3 outward(0, 0, 0), tb(0, 0, 0), tc(0, 0, 0);
            outward[axis] = sign;
            tb[b] = 1;
            tc[c] = 1;
            // Tangents with cross(dpdu, dpdv) = outward normal
            rec.dpdu = sign > 0 ? tb : tc;
            rec.dpdv = sign > 0 ? tc : tb;
            rec.u = dot(rec.p, rec.dpdu) - std::floor(dot(rec.p, rec.dpdu));
            rec.v = dot(rec.p, rec.dpdv) - std::floor(dot(rec.p, rec.dpdv));
            rec.mat = mats[index];
            rec.set_face_normal(r, outward);
            return true;
        }
};

// A model as voxel_grid blocks of up to block^3 cells, skipping blocks with
// no voxel, in a BVH.
inline std::shared_ptr<hittable> voxel_blocks(std::shared_ptr<const vox_model> model,
                                              std::shared_ptr<const voxel_palette> palette, int block = 8) {
    hittable_list blocks;
    for (int z0 = 0; z0 < model->nz; z0 += block)
        for (int y0 = 0; y0 < model->ny; y0 += block)
            for (int x0 = 0; x0 < model->nx; x0 += block) {
                // The bounds of the block's voxels: every surface lies on the
                // faces of a voxel, so a tighter box loses nothing and spares
                // rays walks through the block's empty margin
                int lo[3] = {x0 + block, y0 + block, z0 + block}, hi[3] = {x0, y0, z0};
                for (int z = z0; z < std::min(z0 + block, model->nz); z++)
                    for (int y = y0; y < std::min(y0 + block, model->ny); y++)
                        for (int x = x0; x < std::min(x0 + block, model->nx); x++)
                            if (palette->kind(model->at(x, y, z)) != voxel_kind::empty) {
                                int c[3] = {x, y, z};
                                for (int a = 0; a < 3; a++) {
                                    lo[a] = std::min(lo[a], c[a]);
                                    hi[a] = std::max(hi[a], c[a] + 1);
                                }
                            }
                if (hi[0] > lo[0])
                    blocks.add(std::make_shared<voxel_grid>(model, palette, lo, hi));
            }
    return std::make_shared<bvh>(blocks);
}

#endif
