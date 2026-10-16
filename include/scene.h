#ifndef SCENE_H
#define SCENE_H

#include "camera.h"
#include "hittable_list.h"
#include "material.h"
#include "quad.h"
#include "sphere.h"
#include "texture.h"
#include "tonemap.h"

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

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

    // Reads an optional material name into name; returns nullptr if it is unknown.
    auto read_material = [&](std::istringstream& in, std::string& name) -> std::shared_ptr<material> {
        if (!(in >> name))
            return default_material;
        auto it = materials.find(name);
        return it == materials.end() ? nullptr : it->second;
    };

    // Adds an object to the world, and to the lights if it emits.
    auto add_object = [&](std::shared_ptr<hittable> object, const std::shared_ptr<material>& mat) {
        world.add(object);
        if (std::dynamic_pointer_cast<diffuse_light>(mat))
            lights.add(object);
    };

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
        } else if (keyword == "sphere") {
            double x, y, z, radius;
            if (!(in >> x >> y >> z >> radius))
                return fail(line_number, "expected 'sphere <x> <y> <z> <radius> [material]'");
            std::string mat_name;
            auto mat = read_material(in, mat_name);
            if (!mat)
                return fail(line_number, "unknown material '" + mat_name + "'");
            add_object(std::make_shared<sphere>(point3(x, y, z), radius, mat), mat);
        } else if (keyword == "quad") {
            point3 Q; vec3 u, v;
            if (!(in >> Q[0] >> Q[1] >> Q[2] >> u[0] >> u[1] >> u[2] >> v[0] >> v[1] >> v[2]))
                return fail(line_number, "expected 'quad <Qx Qy Qz> <ux uy uz> <vx vy vz> [material]'");
            if (cross(u, v).near_zero())
                return fail(line_number, "quad edges must not be parallel");
            std::string mat_name;
            auto mat = read_material(in, mat_name);
            if (!mat)
                return fail(line_number, "unknown material '" + mat_name + "'");
            add_object(std::make_shared<quad>(Q, u, v, mat), mat);
        } else if (keyword == "box") {
            point3 a, b;
            if (!(in >> a[0] >> a[1] >> a[2] >> b[0] >> b[1] >> b[2]))
                return fail(line_number, "expected 'box <x0 y0 z0> <x1 y1 z1> [material]'");
            std::string mat_name;
            auto mat = read_material(in, mat_name);
            if (!mat)
                return fail(line_number, "unknown material '" + mat_name + "'");
            add_object(box(a, b, mat), mat);
        } else {
            return fail(line_number, "unknown keyword '" + keyword + "'");
        }

        std::string extra;
        if (in >> extra)
            return fail(line_number, "unexpected value '" + extra + "'");
    }

    return true;
}

#endif
