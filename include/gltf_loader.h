#ifndef GLTF_LOADER_H
#define GLTF_LOADER_H

#include "material.h"
#include "mesh.h"
#include "obj_loader.h"  // obj_part
#include "texture.h"

#include <cgltf.h>

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Reads a glTF 2.0 model (.gltf with its .bin and images, or a single .glb)
// with cgltf, and turns its metallic-roughness materials into ours:
//
//   baseColorFactor x baseColorTexture       principled color
//   metallicFactor x blue, roughnessFactor x green of metallicRoughnessTexture
//                                            principled metallic, roughness
//                                            (glTF's roughness is ours: alpha = r^2)
//   normalTexture (with its scale)           normal_mapped
//   emissiveFactor x KHR_materials_emissive_strength
//                                            emissive (an area light), or with
//                                            an emissiveTexture an emissive_surface
//                                            (glows, but is not sampled as a light)
//   KHR_materials_transmission               thin_dielectric, tinted by the base
//                                            color (glTF's transmission is
//                                            thin-walled without KHR_materials_volume)
//   KHR_materials_ior                        principled / glass ior (default 1.5)
//   KHR_texture_transform                    uv_transform_texture
//   alphaMode MASK or BLEND                  base color alpha as a coverage mask
//
// Other extensions (clearcoat, sheen, iridescence, ...) are listed on std::clog
// and left out. Images inside the file are decoded from memory; image paths
// are relative to the .gltf. Only the first set of texture coordinates is used.
//
// The default scene's node tree is flattened: each node's world matrix is
// applied to its mesh's vertices (normals by the inverse transpose), so every
// instance of a mesh is a copy. Triangles are grouped by material into one
// mesh per material. glTF is y-up and in meters, like our scenes.
//
// With override_mat set, every primitive uses it instead of its own material
// (a clay render). Returns false with a message in error if the file cannot
// be read or has no triangles; bounds gets the box around all triangles.
inline bool load_gltf(const std::string& path, std::shared_ptr<material> override_mat,
                      std::vector<obj_part>& parts, aabb& bounds, std::string& error,
                      bool use_normal_maps = true) {
    cgltf_options options{};
    cgltf_data* raw = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &raw) != cgltf_result_success) {
        error = "cannot read glTF file '" + path + "'";
        return false;
    }
    std::unique_ptr<cgltf_data, void(*)(cgltf_data*)> data(raw, cgltf_free);
    if (cgltf_load_buffers(&options, data.get(), path.c_str()) != cgltf_result_success) {
        error = "cannot load the buffers of '" + path + "'";
        return false;
    }

    auto slash = path.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);

    // Images are decoded once per use: colors as sRGB, data (normals,
    // metallic-roughness) as linear values
    std::map<std::pair<const cgltf_image*, bool>, std::shared_ptr<image_texture>> images;
    auto load_image = [&](const cgltf_texture* tex, bool linear) -> std::shared_ptr<image_texture> {
        if (!tex || !tex->image)
            return nullptr;
        const cgltf_image* image = tex->image;
        auto key = std::make_pair(image, linear);
        if (auto it = images.find(key); it != images.end())
            return it->second;
        // glTF samplers repeat unless told to clamp (33071 = CLAMP_TO_EDGE)
        bool repeat = !(tex->sampler && tex->sampler->wrap_s == 33071 && tex->sampler->wrap_t == 33071);
        std::shared_ptr<image_texture> result;
        if (image->buffer_view) {
            result = std::make_shared<image_texture>(cgltf_buffer_view_data(image->buffer_view),
                                                     image->buffer_view->size, repeat, linear);
        } else if (image->uri && std::string(image->uri).rfind("data:", 0) != 0) {
            std::string uri = image->uri;
            cgltf_decode_uri(uri.data());  // %20 and friends
            uri = std::string(uri.c_str());
            result = std::make_shared<image_texture>(uri[0] == '/' ? uri : dir + uri, repeat, linear);
        }
        if (!result || !result->loaded()) {
            std::clog << "Warning: " << path << ": cannot load image '"
                      << (image->uri ? image->uri : image->name ? image->name : "embedded") << "'\n";
            result = nullptr;
        }
        images[key] = result;
        return result;
    };

    std::set<std::string> ignored;  // Unsupported features, reported once

    // A texture view as a texture: the image, moved by KHR_texture_transform
    auto view_texture = [&](const cgltf_texture_view& view, bool linear) -> std::shared_ptr<texture> {
        int texcoord = view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord;
        if (texcoord != 0) {
            ignored.insert("second texture coordinates");
            return nullptr;
        }
        std::shared_ptr<texture> result = load_image(view.texture, linear);
        if (result && view.has_transform) {
            const auto& t = view.transform;
            result = std::make_shared<uv_transform_texture>(result, t.offset[0], t.offset[1], t.rotation,
                                                            t.scale[0], t.scale[1]);
        }
        return result;
    };

    using surface = std::pair<std::shared_ptr<material>, std::shared_ptr<image_texture>>;
    std::map<const cgltf_material*, surface> converted;
    auto convert = [&](const cgltf_material* m) -> surface {
        if (auto it = converted.find(m); it != converted.end())
            return it->second;
        surface result;
        if (!m) {
            // The spec's default material: white, metallic 1, roughness 1
            result.first = override_mat ? override_mat
                                        : std::make_shared<principled>(std::make_shared<solid_color>(color(1, 1, 1)),
                                                                       1.0, 1.0, 1.5);
            return converted[m] = result;
        }
        const auto& pbr = m->pbr_metallic_roughness;
        color base_factor(pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2]);
        double ior = m->has_ior ? m->ior.ior : 1.5;

        std::shared_ptr<texture> base_image = view_texture(pbr.base_color_texture, false);
        if (m->alpha_mode != cgltf_alpha_mode_opaque && !pbr.base_color_texture.has_transform) {
            auto image = load_image(pbr.base_color_texture.texture, false);
            if (image && image->has_alpha_channel() && !image->fully_opaque())
                result.second = image;
        }
        if (override_mat) {
            result.first = override_mat;
            return converted[m] = result;
        }

        if (m->has_clearcoat && m->clearcoat.clearcoat_factor > 0) ignored.insert("clearcoat");
        if (m->has_sheen) ignored.insert("sheen");
        if (m->has_iridescence) ignored.insert("iridescence");
        if (m->has_volume) ignored.insert("volume");
        if (m->has_specular) ignored.insert("specular");
        if (m->has_anisotropy) ignored.insert("anisotropy");
        if (m->unlit) ignored.insert("unlit");

        std::shared_ptr<texture> base = base_image ? std::shared_ptr<texture>(std::make_shared<scaled_texture>(base_image, base_factor))
                                                   : std::make_shared<solid_color>(base_factor);
        std::shared_ptr<material> mat;
        if (m->has_transmission && m->transmission.transmission_factor > 0) {
            if (m->transmission.transmission_factor < 1 || pbr.roughness_factor > 0)
                ignored.insert("partial or rough transmission");
            mat = std::make_shared<thin_dielectric>(ior, base_factor);
        } else {
            std::shared_ptr<texture> metallic, roughness;
            auto mr = view_texture(pbr.metallic_roughness_texture, true);
            if (mr) {
                metallic = std::make_shared<channel_texture>(mr, 2, pbr.metallic_factor);
                roughness = std::make_shared<channel_texture>(mr, 1, pbr.roughness_factor);
            } else {
                double x = pbr.metallic_factor, r = pbr.roughness_factor;
                metallic = std::make_shared<solid_color>(color(x, x, x));
                roughness = std::make_shared<solid_color>(color(r, r, r));
            }
            mat = std::make_shared<principled>(base, metallic, roughness, ior);
            if (use_normal_maps) {
                if (auto normals = view_texture(m->normal_texture, true)) {
                    if (m->normal_texture.scale != 1)
                        normals = std::make_shared<normal_scale_texture>(normals, m->normal_texture.scale);
                    mat = std::make_shared<normal_mapped>(mat, normals);
                }
            }
        }

        double strength = m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1;
        color emission = strength * color(m->emissive_factor[0], m->emissive_factor[1], m->emissive_factor[2]);
        if (emission.length_squared() > 0) {
            if (m->emissive_texture.texture) {
                if (auto image = view_texture(m->emissive_texture, false))
                    mat = std::make_shared<emissive_surface>(mat, std::make_shared<scaled_texture>(image, emission));
            } else {
                mat = std::make_shared<diffuse_light>(emission);  // A light of the scene
            }
        }
        result.first = mat;
        return converted[m] = result;
    };

    auto geometry = std::make_shared<mesh_data>();
    struct face{ int v[3], n[3], t[3]; };
    std::map<surface, std::vector<face>> faces_by_material;

    auto add_primitive = [&](const cgltf_primitive& prim, const float* m) {
        if (prim.type != cgltf_primitive_type_triangles) {
            ignored.insert("non-triangle primitives");
            return;
        }
        if (prim.has_draco_mesh_compression) {
            ignored.insert("Draco-compressed meshes");
            return;
        }
        const cgltf_accessor *positions = nullptr, *normals = nullptr, *uvs = nullptr;
        for (cgltf_size k = 0; k < prim.attributes_count; k++) {
            const auto& a = prim.attributes[k];
            if (a.type == cgltf_attribute_type_position) positions = a.data;
            else if (a.type == cgltf_attribute_type_normal) normals = a.data;
            else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uvs = a.data;
        }
        if (!positions)
            return;

        // m is column-major: x' = m[0] x + m[4] y + m[8] z + m[12]. Normals
        // take the inverse transpose of the 3x3 part, which is its cofactor
        // matrix divided by the determinant (only the sign of that matters,
        // the normals are normalized later).
        double a00 = m[0], a01 = m[4], a02 = m[8];
        double a10 = m[1], a11 = m[5], a12 = m[9];
        double a20 = m[2], a21 = m[6], a22 = m[10];
        double c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22, c02 = a10 * a21 - a11 * a20;
        double c10 = a02 * a21 - a01 * a22, c11 = a00 * a22 - a02 * a20, c12 = a01 * a20 - a00 * a21;
        double c20 = a01 * a12 - a02 * a11, c21 = a02 * a10 - a00 * a12, c22 = a00 * a11 - a01 * a10;
        double det = a00 * c00 + a01 * c01 + a02 * c02;
        double sign = det < 0 ? -1 : 1;

        int v_base = int(geometry->positions.size());
        int n_base = int(geometry->normals.size());
        int t_base = int(geometry->uvs.size());
        cgltf_size count = positions->count;
        for (cgltf_size k = 0; k < count; k++) {
            float p[3] = {0, 0, 0};
            cgltf_accessor_read_float(positions, k, p, 3);
            geometry->positions.push_back(point3(a00 * p[0] + a01 * p[1] + a02 * p[2] + m[12],
                                                 a10 * p[0] + a11 * p[1] + a12 * p[2] + m[13],
                                                 a20 * p[0] + a21 * p[1] + a22 * p[2] + m[14]));
            bounds = aabb(bounds, aabb(geometry->positions.back(), geometry->positions.back()));
        }
        if (normals && normals->count == count) {
            for (cgltf_size k = 0; k < count; k++) {
                float n[3] = {0, 0, 1};
                cgltf_accessor_read_float(normals, k, n, 3);
                geometry->normals.push_back(sign * vec3(c00 * n[0] + c01 * n[1] + c02 * n[2],
                                                        c10 * n[0] + c11 * n[1] + c12 * n[2],
                                                        c20 * n[0] + c21 * n[1] + c22 * n[2]));
            }
        } else {
            normals = nullptr;
        }
        if (uvs && uvs->count == count) {
            for (cgltf_size k = 0; k < count; k++) {
                float t[2] = {0, 0};
                cgltf_accessor_read_float(uvs, k, t, 2);
                geometry->uvs.push_back(vec3(t[0], 1 - t[1], 0));  // glTF's v runs down the image
            }
        } else {
            uvs = nullptr;
        }

        auto& faces = faces_by_material[convert(prim.material)];
        cgltf_size index_count = prim.indices ? prim.indices->count : count;
        for (cgltf_size k = 0; k + 2 < index_count; k += 3) {
            int idx[3];
            for (int c = 0; c < 3; c++)
                idx[c] = int(prim.indices ? cgltf_accessor_read_index(prim.indices, k + c) : k + c);
            if (det < 0)
                std::swap(idx[1], idx[2]);  // A mirroring matrix flips the winding
            if (idx[0] >= int(count) || idx[1] >= int(count) || idx[2] >= int(count))
                continue;
            face f;
            for (int c = 0; c < 3; c++) {
                f.v[c] = v_base + idx[c];
                f.n[c] = normals ? n_base + idx[c] : -1;
                f.t[c] = uvs ? t_base + idx[c] : -1;
            }
            const auto& p0 = geometry->positions[f.v[0]];
            if (cross(geometry->positions[f.v[1]] - p0, geometry->positions[f.v[2]] - p0).near_zero())
                continue;  // Degenerate
            faces.push_back(f);
        }
    };

    // Walks the node tree of the default scene (or of every root node when
    // the file names no scene)
    std::vector<const cgltf_node*> stack;
    const cgltf_scene* scene = data->scene ? data->scene : data->scenes_count > 0 ? &data->scenes[0] : nullptr;
    if (scene) {
        for (cgltf_size k = 0; k < scene->nodes_count; k++)
            stack.push_back(scene->nodes[k]);
    } else {
        for (cgltf_size k = 0; k < data->nodes_count; k++)
            if (!data->nodes[k].parent)
                stack.push_back(&data->nodes[k]);
    }
    while (!stack.empty()) {
        const cgltf_node* node = stack.back();
        stack.pop_back();
        for (cgltf_size k = 0; k < node->children_count; k++)
            stack.push_back(node->children[k]);
        if (!node->mesh)
            continue;
        if (node->skin || node->mesh->weights_count > 0)
            ignored.insert("skins and morph targets (the rest pose is used)");
        float matrix[16];
        cgltf_node_transform_world(node, matrix);
        for (cgltf_size k = 0; k < node->mesh->primitives_count; k++)
            add_primitive(node->mesh->primitives[k], matrix);
    }

    for (auto& [surf, faces] : faces_by_material) {
        if (faces.empty())
            continue;
        auto& [mat, mask] = surf;
        std::vector<std::shared_ptr<hittable>> triangles;
        triangles.reserve(faces.size());
        for (const auto& f : faces)
            triangles.push_back(std::make_shared<triangle>(geometry.get(), f.v, f.n, f.t, mat.get(), mask.get()));
        parts.push_back({mat, std::make_shared<mesh>(geometry, std::move(triangles), mat, mask)});
    }

    if (!ignored.empty()) {
        std::clog << "Note: " << path << ": not supported, left out:";
        const char* separator = " ";
        for (const auto& feature : ignored) {
            std::clog << separator << feature;
            separator = ", ";
        }
        std::clog << '\n';
    }
    if (parts.empty()) {
        error = "'" + path + "' has no triangles";
        return false;
    }
    return true;
}

#endif
