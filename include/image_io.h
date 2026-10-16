#ifndef IMAGE_IO_H
#define IMAGE_IO_H

#include "color3.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <stb_image_write.h>

// Writes linear pixels as a plain-text PPM (P3), gamma encoded.
inline void write_ppm(std::ostream& out, int width, int height, const std::vector<color>& pixels) {
    out << "P3\n" << width << ' ' << height << "\n255\n";
    for (const auto& pixel : pixels)
        write_color(out, pixel);
}

// Writes linear pixels as an 8-bit PNG, gamma encoded.
inline bool write_png(const std::string& path, int width, int height, const std::vector<color>& pixels) {
    std::vector<unsigned char> bytes;
    bytes.reserve(pixels.size() * 3);
    for (const auto& pixel : pixels) {
        bytes.push_back(to_byte(pixel.x()));
        bytes.push_back(to_byte(pixel.y()));
        bytes.push_back(to_byte(pixel.z()));
    }
    return stbi_write_png(path.c_str(), width, height, 3, bytes.data(), width * 3) != 0;
}

inline bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Writes the image to path, choosing the format from the extension
// (.png or .ppm). An empty path writes PPM to stdout.
inline bool write_image(const std::string& path, int width, int height, const std::vector<color>& pixels) {
    if (path.empty()) {
        write_ppm(std::cout, width, height, pixels);
        return true;
    }
    if (ends_with(path, ".png")) {
        if (write_png(path, width, height, pixels))
            return true;
    } else if (ends_with(path, ".ppm")) {
        std::ofstream out(path);
        if (out) {
            write_ppm(out, width, height, pixels);
            if (out)
                return true;
        }
    } else {
        std::cerr << "Error: unsupported output format '" << path << "' (use .png or .ppm)\n";
        return false;
    }
    std::cerr << "Error: cannot write '" << path << "'\n";
    return false;
}

#endif
