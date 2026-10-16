#ifndef SCENE_H
#define SCENE_H

#include "camera.h"
#include "hittable_list.h"
#include "sphere.h"

#include <fstream>
#include <iostream>
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
//   sphere       0 0 -1  0.5   # center x y z, radius
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
        } else if (keyword == "sphere") {
            double x, y, z, radius;
            if (!(in >> x >> y >> z >> radius))
                return fail(line_number, "expected 'sphere <x> <y> <z> <radius>'");
            world.add(std::make_shared<sphere>(point3(x, y, z), radius));
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
