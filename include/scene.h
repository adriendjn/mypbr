#ifndef SCENE_H
#define SCENE_H

#include "camera.h"
#include "hittable_list.h"
#include "instance.h"
#include "material.h"
#include "obj_loader.h"
#include "quad.h"
#include "sphere.h"
#include "texture.h"
#include "tonemap.h"
#include "transform.h"

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>

// Everything a scene file describes.
class scene{
    public:
        hittable_list    world;
        hittable_list    lights;   // Emissive objects, also part of world
        camera           cam;
        display_settings display;  // How radiance becomes PNG/PPM pixels
};

// Loads a scene description.
//
// The file is read line by line. Each line starts with a keyword followed by
// its values; blank lines and anything after '#' are ignored:
//
//   image_width  800
//   aspect_ratio 16/9          # or a plain number, e.g. 1.5
//   samples_per_pixel 100      # random samples per pixel (antialiasing)
//   max_depth    50            # hard cap on bounces (Russian roulette ends most paths earlier)
//
//   camera lookfrom 13 2 3  lookat 0 0 0  vup 0 1 0  vfov 20
//   aperture     0.6           # defocus angle in degrees (0 = pinhole)
//   focus        10            # distance to the plane of perfect focus
//
// The camera line takes any of its settings, in any order; aperture and
// focus can also be given on it.
//
//   texture  earth  image textures/earthmap.jpg   # path relative to the scene file
//   texture  tiles  checker 0.5  0.2 0.3 0.1  0.9 0.9 0.9   # cube size, two colors
//   texture  plain  solid 0.8 0.2 0.2
//
//   material ground lambertian 0.8 0.8 0.0    # albedo r g b ...
//   material globe  lambertian texture earth  # ... or a named texture
//   material gold   metal 0.8 0.6 0.2 0.3     # albedo r g b, roughness (0 = mirror)
//   material glass  dielectric 1.5            # index of refraction
//   material frost  dielectric 1.5 roughness 0.3
//   material paint  principled color 0.8 0.1 0.1 metallic 0 roughness 0.4 ior 1.5
//
// principled settings are all optional and can come in any order (defaults:
// color 0.8 0.8 0.8, metallic 0, roughness 0.5, ior 1.5); "texture <name>"
// can replace "color r g b".
//   material lamp   emissive 15 15 15         # area light, radiance r g b
//
//   background   0 0 0         # constant background instead of the sky gradient
//   exposure     0             # stops; display images are scaled by 2^exposure
//   tonemap      aces          # none (clamp, the default), reinhard or aces
//
//   sphere       0 0 -1  0.5  glass   # center x y z, radius, [material]
//   quad   Qx Qy Qz  ux uy uz  vx vy vz  [material]   # corner, two edges
//   box    x0 y0 z0  x1 y1 z1  [material]            # two opposite corners
//   mesh   models/bunny.obj  [smooth] [material]      # Wavefront OBJ, path relative
//                                                     # to the scene file; smooth
//                                                     # makes vertex normals if the
//                                                     # file has none
//
// Any object can be followed by transforms, applied in the order given:
//
//   box 0 0 0  165 330 165  white  rotate_y 15  translate 265 0 295
//   mesh bunny.obj glass  scale 2000  rotate 30 0 1 0  translate 278 0 278
//
//   translate x y z | scale s | scale sx sy sz | rotate_x|rotate_y|rotate_z deg
//   rotate deg ax ay az   # about an arbitrary axis
//
// Transformed objects are instances sharing the original geometry; a mesh
// file used several times with the same material is loaded once. In a mesh,
// "usemtl <name>" selects the scene material of that name when there is one.
//
// Materials must be defined before they are used. A sphere without a
// material name gets a default grey Lambertian.
//
// Every object with an emissive material is also added to lights, so the
// renderer can sample it directly.
//
// Returns false and prints the offending line on std::cerr on error.
inline bool load_scene(const std::string& path, scene& result) {
    auto& world = result.world;
    auto& lights = result.lights;
    auto& cam = result.cam;

    std::ifstream file(path);
    if (!file) {
        std::cerr << "Error: cannot open scene file '" << path << "'\n";
        return false;
    }

    auto fail = [&](int line_number, const std::string& message) {
        std::cerr << path << ":" << line_number << ": " << message << '\n';
        return false;
    };

    std::map<std::string, std::shared_ptr<material>> materials;
    std::map<std::string, std::shared_ptr<texture>> textures;

    // Directory of the scene file, for resolving relative texture paths.
    auto slash = path.find_last_of('/');
    std::string scene_dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);

    // Reads either "r g b" or "texture <name>" into tex, where first is the
    // token already read ("texture" or the red value); false if neither works.
    auto read_color_or_texture = [&](std::istringstream& in, const std::string& first,
                                     std::shared_ptr<texture>& tex, std::string& error) {
        if (first == "texture") {
            std::string name;
            if (!(in >> name))
                return false;
            auto it = textures.find(name);
            if (it == textures.end()) {
                error = "unknown texture '" + name + "'";
                return false;
            }
            tex = it->second;
            return true;
        }
        std::istringstream rgb(first);
        double r, g, b;
        if (!(rgb >> r) || !(in >> g >> b))
            return false;
        tex = std::make_shared<solid_color>(color(r, g, b));
        return true;
    };
    auto default_material = std::make_shared<lambertian>(color(0.5, 0.5, 0.5));

    // Reads what may follow an object's geometry: an optional material name,
    // then any transforms, applied in the order given. Returns false with a
    // message in error on a problem.
    auto read_tail = [&](std::istringstream& in, std::shared_ptr<material>& mat, transform& xform,
                         bool& transformed, std::string& error) {
        mat = default_material;
        xform = transform();
        transformed = false;

        auto is_transform = [](const std::string& t) {
            return t == "translate" || t == "scale" || t == "rotate"
                || t == "rotate_x" || t == "rotate_y" || t == "rotate_z";
        };

        std::string token;
        if (!(in >> token))
            return true;
        if (!is_transform(token)) {
            auto it = materials.find(token);
            if (it == materials.end()) {
                error = "unknown material '" + token + "'";
                return false;
            }
            mat = it->second;
            if (!(in >> token))
                return true;
        }

        do {
            transform step;
            bool ok = true;
            if (token == "translate") {
                vec3 d;
                ok = bool(in >> d[0] >> d[1] >> d[2]);
                step = transform::translate(d);
            } else if (token == "rotate_x" || token == "rotate_y" || token == "rotate_z") {
                double degrees;
                ok = bool(in >> degrees);
                vec3 axis(token == "rotate_x", token == "rotate_y", token == "rotate_z");
                step = transform::rotate(degrees, axis);
            } else if (token == "rotate") {
                double degrees;
                vec3 axis;
                ok = bool(in >> degrees >> axis[0] >> axis[1] >> axis[2]) && axis.length_squared() > 0;
                if (ok)
                    step = transform::rotate(degrees, axis);
            } else if (token == "scale") {
                // "scale s" or "scale sx sy sz"
                vec3 f;
                ok = bool(in >> f[0]);
                if (ok) {
                    // A second number means all three must be there
                    std::streampos mark = in.tellg();
                    if (in >> f[1]) {
                        ok = bool(in >> f[2]);
                    } else {
                        in.clear();
                        in.seekg(mark);
                        f = vec3(f[0], f[0], f[0]);
                    }
                }
                ok = ok && f[0] != 0 && f[1] != 0 && f[2] != 0;
                step = transform::scale(f);
            } else {
                error = "unexpected value '" + token + "' (expected a transform: translate, scale, "
                        "rotate, rotate_x, rotate_y or rotate_z)";
                return false;
            }
            if (!ok) {
                error = "bad values for '" + token + "'";
                return false;
            }
            xform = step.then_after(xform);
            transformed = true;
        } while (in >> token);
        return true;
    };

    // Adds an object to the world, and to the lights if it emits.
    auto add_object = [&](std::shared_ptr<hittable> object, const std::shared_ptr<material>& mat,
                          const transform& xform, bool transformed, std::string& error) {
        bool emissive = bool(std::dynamic_pointer_cast<diffuse_light>(mat));
        if (transformed) {
            if (emissive && !xform.is_similarity()) {
                error = "lights can only be moved, rotated and scaled uniformly";
                return false;
            }
            object = std::make_shared<instance>(object, xform);
        }
        world.add(object);
        if (emissive)
            lights.add(object);
        return true;
    };

    // Meshes already loaded, by file and default material, so repeated
    // "mesh" lines share geometry and only add instances.
    std::map<std::tuple<std::string, bool, const material*>, std::vector<obj_part>> mesh_cache;

    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        line_number++;

        auto comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);

        std::istringstream in(line);
        std::string keyword;
        if (!(in >> keyword))
            continue;  // Empty line

        if (keyword == "image_width") {
            if (!(in >> cam.image_width) || cam.image_width < 1)
                return fail(line_number, "expected 'image_width <positive integer>'");
        } else if (keyword == "aspect_ratio") {
            // Accepts "16/9" as well as a plain number
            double num, den = 1.0;
            if (!(in >> num))
                return fail(line_number, "expected 'aspect_ratio <w>/<h>' or 'aspect_ratio <number>'");
            if (in.peek() == '/') {
                in.get();
                if (!(in >> den))
                    return fail(line_number, "expected 'aspect_ratio <w>/<h>'");
            }
            if (num <= 0 || den <= 0)
                return fail(line_number, "aspect_ratio must be positive");
            cam.aspect_ratio = num / den;
        } else if (keyword == "samples_per_pixel") {
            if (!(in >> cam.samples_per_pixel) || cam.samples_per_pixel < 1)
                return fail(line_number, "expected 'samples_per_pixel <positive integer>'");
        } else if (keyword == "max_depth") {
            if (!(in >> cam.max_depth) || cam.max_depth < 1)
                return fail(line_number, "expected 'max_depth <positive integer>'");
        } else if (keyword == "camera") {
            std::string key;
            while (in >> key) {
                bool ok = true;
                if (key == "lookfrom")      ok = bool(in >> cam.lookfrom[0] >> cam.lookfrom[1] >> cam.lookfrom[2]);
                else if (key == "lookat")   ok = bool(in >> cam.lookat[0] >> cam.lookat[1] >> cam.lookat[2]);
                else if (key == "vup")      ok = bool(in >> cam.vup[0] >> cam.vup[1] >> cam.vup[2]);
                else if (key == "vfov")     ok = bool(in >> cam.vfov) && cam.vfov > 0 && cam.vfov < 180;
                else if (key == "aperture") ok = bool(in >> cam.defocus_angle) && cam.defocus_angle >= 0;
                else if (key == "focus")    ok = bool(in >> cam.focus_dist) && cam.focus_dist > 0;
                else return fail(line_number, "unknown camera setting '" + key + "'");
                if (!ok)
                    return fail(line_number, "bad value for camera setting '" + key + "'");
            }
        } else if (keyword == "vfov") {
            if (!(in >> cam.vfov) || cam.vfov <= 0 || cam.vfov >= 180)
                return fail(line_number, "expected 'vfov <degrees between 0 and 180>'");
        } else if (keyword == "aperture") {
            if (!(in >> cam.defocus_angle) || cam.defocus_angle < 0)
                return fail(line_number, "expected 'aperture <defocus angle in degrees>'");
        } else if (keyword == "focus") {
            if (!(in >> cam.focus_dist) || cam.focus_dist <= 0)
                return fail(line_number, "expected 'focus <positive distance>'");
        } else if (keyword == "background") {
            double r, g, b;
            if (!(in >> r >> g >> b))
                return fail(line_number, "expected 'background <r> <g> <b>'");
            cam.use_sky = false;
            cam.background = color(r, g, b);
        } else if (keyword == "exposure") {
            if (!(in >> result.display.exposure))
                return fail(line_number, "expected 'exposure <stops>'");
        } else if (keyword == "tonemap") {
            std::string name;
            if (!(in >> name) || !result.display.set_tonemapper(name))
                return fail(line_number, "expected 'tonemap none|reinhard|aces'");
        } else if (keyword == "texture") {
            std::string name, type;
            if (!(in >> name >> type))
                return fail(line_number, "expected 'texture <name> <type> <parameters...>'");

            std::shared_ptr<texture> tex;
            if (type == "solid") {
                double r, g, b;
                if (!(in >> r >> g >> b))
                    return fail(line_number, "expected 'texture <name> solid <r> <g> <b>'");
                tex = std::make_shared<solid_color>(color(r, g, b));
            } else if (type == "checker") {
                double scale, r0, g0, b0, r1, g1, b1;
                if (!(in >> scale >> r0 >> g0 >> b0 >> r1 >> g1 >> b1) || scale <= 0)
                    return fail(line_number, "expected 'texture <name> checker <scale> <r g b> <r g b>'");
                tex = std::make_shared<checker_texture>(scale,
                    std::make_shared<solid_color>(color(r0, g0, b0)),
                    std::make_shared<solid_color>(color(r1, g1, b1)));
            } else if (type == "image") {
                std::string file;
                if (!(in >> file))
                    return fail(line_number, "expected 'texture <name> image <path>'");
                if (file[0] != '/')
                    file = scene_dir + file;
                auto image = std::make_shared<image_texture>(file);
                if (!image->loaded())
                    return fail(line_number, "cannot load image '" + file + "'");
                tex = image;
            } else {
                return fail(line_number, "unknown texture type '" + type + "'");
            }
            textures[name] = tex;
        } else if (keyword == "material") {
            std::string name, type;
            if (!(in >> name >> type))
                return fail(line_number, "expected 'material <name> <type> <parameters...>'");

            std::shared_ptr<material> mat;
            if (type == "lambertian") {
                std::shared_ptr<texture> tex;
                std::string first, error;
                if (!(in >> first) || !read_color_or_texture(in, first, tex, error))
                    return fail(line_number, error.empty()
                        ? "expected 'material <name> lambertian <r> <g> <b>' or '... lambertian texture <name>'"
                        : error);
                mat = std::make_shared<lambertian>(tex);
            } else if (type == "metal") {
                double r, g, b, roughness;
                if (!(in >> r >> g >> b >> roughness) || roughness < 0)
                    return fail(line_number, "expected 'material <name> metal <r> <g> <b> <roughness>'");
                mat = std::make_shared<metal>(color(r, g, b), roughness);
            } else if (type == "dielectric") {
                double ior, roughness = 0;
                std::string key;
                if (!(in >> ior) || ior <= 0)
                    return fail(line_number, "expected 'material <name> dielectric <index of refraction> [roughness <r>]'");
                if (in >> key) {
                    if (key != "roughness" || !(in >> roughness) || roughness < 0)
                        return fail(line_number, "expected 'material <name> dielectric <index of refraction> [roughness <r>]'");
                }
                mat = std::make_shared<dielectric>(ior, roughness);
            } else if (type == "principled") {
                std::shared_ptr<texture> base = std::make_shared<solid_color>(color(0.8, 0.8, 0.8));
                double metallic = 0, roughness = 0.5, ior = 1.5;
                std::string key, error;
                while (in >> key) {
                    bool ok = true;
                    if (key == "texture") {
                        ok = read_color_or_texture(in, key, base, error);
                    } else if (key == "color") {
                        std::string first;
                        ok = bool(in >> first) && read_color_or_texture(in, first, base, error);
                    }
                    else if (key == "metallic")  ok = bool(in >> metallic) && metallic >= 0 && metallic <= 1;
                    else if (key == "roughness") ok = bool(in >> roughness) && roughness >= 0 && roughness <= 1;
                    else if (key == "ior")       ok = bool(in >> ior) && ior > 0;
                    else return fail(line_number, "unknown principled setting '" + key + "'");
                    if (!ok)
                        return fail(line_number, error.empty() ? "bad value for principled setting '" + key + "'" : error);
                }
                mat = std::make_shared<principled>(base, metallic, roughness, ior);
            } else if (type == "emissive") {
                double r, g, b;
                if (!(in >> r >> g >> b))
                    return fail(line_number, "expected 'material <name> emissive <r> <g> <b>'");
                mat = std::make_shared<diffuse_light>(color(r, g, b));
            } else {
                return fail(line_number, "unknown material type '" + type + "'");
            }
            materials[name] = mat;
        } else if (keyword == "sphere" || keyword == "quad" || keyword == "box") {
            std::shared_ptr<hittable> object;
            std::shared_ptr<material> mat;
            transform xform;
            bool transformed;
            std::string error;

            if (keyword == "sphere") {
                double x, y, z, radius;
                if (!(in >> x >> y >> z >> radius) || radius == 0)
                    return fail(line_number, "expected 'sphere <x> <y> <z> <non-zero radius> [material] [transforms]'");
                if (!read_tail(in, mat, xform, transformed, error))
                    return fail(line_number, error);
                object = std::make_shared<sphere>(point3(x, y, z), radius, mat);
            } else if (keyword == "quad") {
                point3 Q; vec3 u, v;
                if (!(in >> Q[0] >> Q[1] >> Q[2] >> u[0] >> u[1] >> u[2] >> v[0] >> v[1] >> v[2]))
                    return fail(line_number, "expected 'quad <Qx Qy Qz> <ux uy uz> <vx vy vz> [material] [transforms]'");
                if (cross(u, v).near_zero())
                    return fail(line_number, "quad edges must not be parallel");
                if (!read_tail(in, mat, xform, transformed, error))
                    return fail(line_number, error);
                object = std::make_shared<quad>(Q, u, v, mat);
            } else {
                point3 a, b;
                if (!(in >> a[0] >> a[1] >> a[2] >> b[0] >> b[1] >> b[2]))
                    return fail(line_number, "expected 'box <x0 y0 z0> <x1 y1 z1> [material] [transforms]'");
                if (!read_tail(in, mat, xform, transformed, error))
                    return fail(line_number, error);
                object = box(a, b, mat);
            }
            if (!add_object(object, mat, xform, transformed, error))
                return fail(line_number, error);
        } else if (keyword == "mesh") {
            std::string file, error;
            std::shared_ptr<material> mat;
            transform xform;
            bool transformed;
            if (!(in >> file))
                return fail(line_number, "expected 'mesh <file.obj> [material] [transforms]'");
            if (file[0] != '/')
                file = scene_dir + file;
            // An optional "smooth" comes before the usual material and transforms
            std::string rest, first;
            std::getline(in, rest);
            std::istringstream peek(rest);
            bool smooth = (peek >> first) && first == "smooth";
            std::istringstream tail(smooth ? rest.substr(rest.find("smooth") + 6) : rest);
            if (!read_tail(tail, mat, xform, transformed, error))
                return fail(line_number, error);

            auto key = std::make_tuple(file, smooth, mat.get());
            auto cached = mesh_cache.find(key);
            if (cached == mesh_cache.end()) {
                std::vector<obj_part> parts;
                if (!load_obj(file, materials, mat, smooth, parts, error))
                    return fail(line_number, error);
                size_t triangle_count = 0;
                for (const auto& part : parts)
                    triangle_count += part.geometry->size();
                std::clog << "Loaded " << file << ": " << triangle_count << " triangles\n";
                cached = mesh_cache.emplace(key, std::move(parts)).first;
            }
            for (const auto& part : cached->second)
                if (!add_object(part.geometry, part.mat, xform, transformed, error))
                    return fail(line_number, error);
        } else {
            return fail(line_number, "unknown keyword '" + keyword + "'");
        }

        std::string extra;
        if (in >> extra)
            return fail(line_number, "unexpected value '" + extra + "'");
    }

    if (world.empty())
        return fail(line_number, "the scene has no objects");
    if ((cam.lookfrom - cam.lookat).near_zero() || cross(cam.vup, cam.lookfrom - cam.lookat).near_zero())
        return fail(line_number, "camera lookfrom must differ from lookat, and vup must not point along the view");

    return true;
}

#endif
