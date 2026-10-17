#ifndef OBJ_LOADER_H
#define OBJ_LOADER_H

#include "material.h"
#include "mesh.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// A part of an OBJ file that uses one material.
class obj_part{
    public:
        std::shared_ptr<material> mat;
        std::shared_ptr<mesh> geometry;
};

// A material read from an MTL file, with the alpha mask that cuts holes in
// the surfaces using it (null when they are opaque).
class mtl_material{
    public:
        std::shared_ptr<material> mat;
        std::shared_ptr<image_texture> alpha_mask;
};

// Reads a Wavefront MTL file and adds its materials to result, mapped onto
// the renderer's materials:
//
//   Kd r g b        diffuse color          -> lambertian
//   map_Kd file     diffuse texture        -> replaces Kd
//   Ks r g b, Ns n  specular color and exponent: a non-black Ks gives a
//                   principled material, glossy with roughness from Ns
//   Ni ior          index of refraction of that glossy layer (default 1.5)
//   Ke r g b        emission               -> emissive (an area light)
//   map_d file      alpha mask (its alpha channel, or grey level)
//
// Without map_d, a diffuse texture's alpha channel is used as the mask, a
// common convention (Sponza's plants and chains rely on it). The scalar "d"
// is ignored: exporters often write "d 0" for opaque materials.
//
// Texture paths are relative to the MTL file; textures wrap around (repeat).
// Options before a texture file (-s, -o, -bm, ...) are skipped by taking the
// last word as the file name. A texture that cannot be loaded is reported on
// std::clog and the material falls back to its plain color. Other statements
// (Ka, illum, map_Bump, ...) are ignored. Returns false only if the file
// cannot be opened.
inline bool load_mtl(const std::string& path, std::map<std::string, mtl_material>& result,
                     std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open '" + path + "'";
        return false;
    }
    auto slash = path.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);

    // Images shared between materials are loaded once
    std::map<std::string, std::shared_ptr<image_texture>> images;
    auto load_image = [&](const std::string& name) -> std::shared_ptr<image_texture> {
        std::string full = name[0] == '/' ? name : dir + name;
        auto it = images.find(full);
        if (it != images.end())
            return it->second;
        auto image = std::make_shared<image_texture>(full, true);
        if (!image->loaded()) {
            std::clog << "Warning: " << path << ": cannot load texture '" << full << "'\n";
            image = nullptr;
        }
        images[full] = image;
        return image;
    };

    struct entry{
        color kd{0.8, 0.8, 0.8}, ks{0, 0, 0}, ke{0, 0, 0};
        double ns = 0, ni = 1.5;
        std::shared_ptr<image_texture> map_kd, map_d;
    };
    std::vector<std::pair<std::string, entry>> entries;

    std::string line;
    while (std::getline(file, line)) {
        std::istringstream in(line);
        std::string keyword;
        if (!(in >> keyword) || keyword[0] == '#')
            continue;
        if (keyword == "newmtl") {
            std::string name;
            in >> name;
            entries.push_back({name, entry()});
            continue;
        }
        if (entries.empty())
            continue;
        auto& e = entries.back().second;
        auto read_color = [&](color& c) {
            double r, g, b;
            if (in >> r) {
                c = color(r, r, r);  // A single value means grey
                if (in >> g >> b)
                    c = color(r, g, b);
            }
        };
        auto last_word = [&]() {
            std::string word, last;
            while (in >> word)
                last = word;
            return last;
        };
        if (keyword == "Kd") read_color(e.kd);
        else if (keyword == "Ks") read_color(e.ks);
        else if (keyword == "Ke") read_color(e.ke);
        else if (keyword == "Ns") in >> e.ns;
        else if (keyword == "Ni") in >> e.ni;
        else if (keyword == "map_Kd") { auto f = last_word(); if (!f.empty()) e.map_kd = load_image(f); }
        else if (keyword == "map_d")  { auto f = last_word(); if (!f.empty()) e.map_d = load_image(f); }
    }

    for (auto& [name, e] : entries) {
        mtl_material m;
        std::shared_ptr<texture> base = e.map_kd ? std::static_pointer_cast<texture>(e.map_kd)
                                                 : std::make_shared<solid_color>(e.kd);
        if (e.ke.length_squared() > 0) {
            m.mat = std::make_shared<diffuse_light>(e.ke);
        } else if (e.ks.length_squared() > 0) {
            // A Phong exponent n has the spread of a microfacet alpha of
            // sqrt(2 / (n + 2)); our roughness is sqrt(alpha).
            double roughness = std::sqrt(std::sqrt(2 / (std::fmax(e.ns, 0.0) + 2)));
            double ior = e.ni > 1 ? e.ni : 1.5;
            m.mat = std::make_shared<principled>(base, 0.0, roughness, ior);
        } else {
            m.mat = std::make_shared<lambertian>(base);
        }

        if (e.map_d && !e.map_d->fully_opaque())
            m.alpha_mask = e.map_d;
        else if (!e.map_d && e.map_kd && e.map_kd->has_alpha_channel() && !e.map_kd->fully_opaque())
            m.alpha_mask = e.map_kd;
        result[name] = m;
    }
    return true;
}

// Minimal Wavefront OBJ reader: v, vn, vt and f (polygons are triangulated
// as fans; v, v/t, v//n and v/t/n forms; negative indices count back from
// the end), plus mtllib and usemtl. Everything else (o, g, s, ...) is ignored.
//
// Faces are grouped by material. A usemtl name selects, in order: the scene
// material of that name, the material of that name from the OBJ's MTL
// files (see load_mtl; paths relative to the OBJ), or default_mat. A missing
// MTL file only gives a warning. Returns one part per material used, or
// false with a message in error.
//
// With smooth set, a file without vertex normals gets area-weighted averages
// of the face normals around each vertex, for smooth shading of scans.
inline bool load_obj(const std::string& path,
                     const std::map<std::string, std::shared_ptr<material>>& materials,
                     std::shared_ptr<material> default_mat, bool smooth,
                     std::vector<obj_part>& parts, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open '" + path + "'";
        return false;
    }

    auto data = std::make_shared<mesh_data>();
    struct face{ int v[3], n[3], t[3]; };
    using surface = std::pair<std::shared_ptr<material>, std::shared_ptr<image_texture>>;
    std::map<surface, std::vector<face>> faces_by_material;
    surface current{default_mat, nullptr};

    auto slash = path.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
    std::map<std::string, mtl_material> file_materials;

    // The rest of the line after a keyword, without surrounding blanks.
    auto argument = [](const char* p) {
        std::string arg(p);
        arg.erase(0, arg.find_first_not_of(" \t"));
        arg.erase(arg.find_last_not_of(" \t\r") + 1);
        return arg;
    };

    // Resolves a 1-based (or negative, relative) OBJ index; -1 if absent or invalid.
    auto resolve = [](long index, size_t count) -> int {
        if (index > 0 && size_t(index) <= count) return int(index - 1);
        if (index < 0 && size_t(-index) <= count) return int(long(count) + index);
        return -1;
    };

    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        line_number++;
        const char* s = line.c_str();
        while (*s == ' ' || *s == '\t') s++;

        auto read_vec = [&](const char* p, int n) {
            double x[3] = {0, 0, 0};
            char* end;
            for (int k = 0; k < n; k++) {
                x[k] = std::strtod(p, &end);
                p = end;
            }
            return vec3(x[0], x[1], x[2]);
        };

        if (s[0] == 'v' && (s[1] == ' ' || s[1] == '\t')) {
            data->positions.push_back(read_vec(s + 2, 3));
        } else if (s[0] == 'v' && s[1] == 'n') {
            data->normals.push_back(read_vec(s + 2, 3));
        } else if (s[0] == 'v' && s[1] == 't') {
            data->uvs.push_back(read_vec(s + 2, 2));
        } else if (s[0] == 'f' && (s[1] == ' ' || s[1] == '\t')) {
            // Parse every vertex of the polygon
            std::vector<int> vs, ns, ts;
            const char* p = s + 2;
            while (true) {
                while (*p == ' ' || *p == '\t' || *p == '\r') p++;
                if (*p == '\0') break;
                char* end;
                long v = std::strtol(p, &end, 10), t = 0, n = 0;
                if (end == p) {
                    error = path + ":" + std::to_string(line_number) + ": bad face";
                    return false;
                }
                p = end;
                if (*p == '/') {
                    p++;
                    if (*p != '/') { t = std::strtol(p, &end, 10); p = end; }
                    if (*p == '/') { p++; n = std::strtol(p, &end, 10); p = end; }
                }
                int vi = resolve(v, data->positions.size());
                if (vi < 0) {
                    error = path + ":" + std::to_string(line_number) + ": vertex index out of range";
                    return false;
                }
                vs.push_back(vi);
                ts.push_back(resolve(t, data->uvs.size()));
                ns.push_back(resolve(n, data->normals.size()));
            }
            for (size_t k = 1; k + 1 < vs.size(); k++) {
                face f;
                size_t idx[3] = {0, k, k + 1};
                for (int c = 0; c < 3; c++) {
                    f.v[c] = vs[idx[c]];
                    f.n[c] = ns[idx[c]];
                    f.t[c] = ts[idx[c]];
                }
                // Skip degenerate triangles
                const auto& p0 = data->positions[f.v[0]];
                if (cross(data->positions[f.v[1]] - p0, data->positions[f.v[2]] - p0).near_zero())
                    continue;
                faces_by_material[current].push_back(f);
            }
        } else if (line.compare(s - line.c_str(), 6, "usemtl") == 0) {
            std::string name = argument(s + 6);
            if (auto it = materials.find(name); it != materials.end())
                current = {it->second, nullptr};
            else if (auto mtl = file_materials.find(name); mtl != file_materials.end())
                current = {mtl->second.mat, mtl->second.alpha_mask};
            else
                current = {default_mat, nullptr};
        } else if (line.compare(s - line.c_str(), 6, "mtllib") == 0) {
            // Usually one file name; several are allowed
            std::istringstream names(argument(s + 6));
            std::string name, mtl_error;
            while (names >> name)
                if (!load_mtl(name[0] == '/' ? name : dir + name, file_materials, mtl_error))
                    std::clog << "Warning: " << path << ": " << mtl_error << '\n';
        }
    }

    if (smooth && data->normals.empty()) {
        data->normals.assign(data->positions.size(), vec3(0, 0, 0));
        for (auto& [surf, faces] : faces_by_material) {
            for (auto& f : faces) {
                const auto& p0 = data->positions[f.v[0]];
                // Unnormalized cross product: larger faces weigh more
                vec3 n = cross(data->positions[f.v[1]] - p0, data->positions[f.v[2]] - p0);
                for (int c = 0; c < 3; c++) {
                    data->normals[f.v[c]] += n;
                    f.n[c] = f.v[c];
                }
            }
        }
    }

    for (auto& [surf, faces] : faces_by_material) {
        auto& [mat, mask] = surf;
        std::vector<std::shared_ptr<hittable>> triangles;
        triangles.reserve(faces.size());
        for (const auto& f : faces)
            triangles.push_back(std::make_shared<triangle>(data.get(), f.v, f.n, f.t, mat.get(), mask.get()));
        parts.push_back({mat, std::make_shared<mesh>(data, std::move(triangles), mat, mask)});
    }

    if (parts.empty()) {
        error = "'" + path + "' has no faces";
        return false;
    }
    return true;
}

#endif
