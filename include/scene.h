#ifndef SCENE_H
#define SCENE_H

#include "camera.h"
#include "hittable_list.h"
#include "material.h"
#include "sphere.h"

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

// Loads a scene description into a world and a camera.
//
// The file is read line by line. Each line starts with a keyword followed by
// its values; blank lines and anything after '#' are ignored:
//
//   image_width  800
//   aspect_ratio 16/9          # or a plain number, e.g. 1.5
//   samples_per_pixel 100      # random samples per pixel (antialiasing)
//   max_depth    50            # maximum number of ray bounces
//
//   material ground lambertian 0.8 0.8 0.0    # albedo r g b
//   material gold   metal 0.8 0.6 0.2 0.3     # albedo r g b, fuzz
//   material glass  dielectric 1.5            # index of refraction
//
//   sphere       0 0 -1  0.5  glass   # center x y z, radius, [material]
//
// Materials must be defined before they are used. A sphere without a
// material name gets a default grey Lambertian.
//
// Returns false and prints the offending line on std::cerr on error.
inline bool load_scene(const std::string& path, hittable_list& world, camera& cam) {
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
    auto default_material = std::make_shared<lambertian>(color(0.5, 0.5, 0.5));

    // Reads an optional material name into name; returns nullptr if it is unknown.
    auto read_material = [&](std::istringstream& in, std::string& name) -> std::shared_ptr<material> {
        if (!(in >> name))
            return default_material;
        auto it = materials.find(name);
        return it == materials.end() ? nullptr : it->second;
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
        } else if (keyword == "material") {
            std::string name, type;
            if (!(in >> name >> type))
                return fail(line_number, "expected 'material <name> <type> <parameters...>'");

            std::shared_ptr<material> mat;
            if (type == "lambertian") {
                double r, g, b;
                if (!(in >> r >> g >> b))
                    return fail(line_number, "expected 'material <name> lambertian <r> <g> <b>'");
                mat = std::make_shared<lambertian>(color(r, g, b));
            } else if (type == "metal") {
                double r, g, b, fuzz;
                if (!(in >> r >> g >> b >> fuzz))
                    return fail(line_number, "expected 'material <name> metal <r> <g> <b> <fuzz>'");
                mat = std::make_shared<metal>(color(r, g, b), fuzz);
            } else if (type == "dielectric") {
                double ior;
                if (!(in >> ior) || ior <= 0)
                    return fail(line_number, "expected 'material <name> dielectric <index of refraction>'");
                mat = std::make_shared<dielectric>(ior);
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
            world.add(std::make_shared<sphere>(point3(x, y, z), radius, mat));
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
