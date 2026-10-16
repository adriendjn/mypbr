#ifndef OBJ_LOADER_H
#define OBJ_LOADER_H

#include "material.h"
#include "mesh.h"

#include <cstdlib>
#include <fstream>
#include <map>
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

// Minimal Wavefront OBJ reader: v, vn, vt and f (polygons are triangulated
// as fans; v, v/t, v//n and v/t/n forms; negative indices count back from
// the end), plus usemtl. Everything else (o, g, s, mtllib, ...) is ignored.
//
// Faces are grouped by material: a usemtl name that matches a scene material
// selects it, any other name (or none) uses default_mat. Returns one part per
// material used, or false with a message in error.
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
    std::map<std::shared_ptr<material>, std::vector<face>> faces_by_material;
    auto current = default_mat;

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
            std::string name(s + 6);
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t\r") + 1);
            auto it = materials.find(name);
            current = it == materials.end() ? default_mat : it->second;
        }
    }

    if (smooth && data->normals.empty()) {
        data->normals.assign(data->positions.size(), vec3(0, 0, 0));
        for (auto& [mat, faces] : faces_by_material) {
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

    for (auto& [mat, faces] : faces_by_material) {
        std::vector<std::shared_ptr<hittable>> triangles;
        triangles.reserve(faces.size());
        for (const auto& f : faces)
            triangles.push_back(std::make_shared<triangle>(data.get(), f.v, f.n, f.t, mat.get()));
        parts.push_back({mat, std::make_shared<mesh>(data, std::move(triangles), mat)});
    }

    if (parts.empty()) {
        error = "'" + path + "' has no faces";
        return false;
    }
    return true;
}

#endif
