#ifndef IMAGE_IO_H
#define IMAGE_IO_H

#include "color3.h"
#include "tonemap.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <stb_image_write.h>

// Writes pixels as a plain-text PPM (P3) after exposure, tonemapping and the sRGB curve.
inline void write_ppm(std::ostream& out, int width, int height, const std::vector<color>& pixels,
                      const display_settings& display) {
    out << "P3\n" << width << ' ' << height << "\n255\n";
    for (const auto& pixel : pixels)
        write_color(out, display.apply(pixel));
}

// Writes pixels as an 8-bit PNG after exposure, tonemapping and the sRGB curve.
inline bool write_png(const std::string& path, int width, int height, const std::vector<color>& pixels,
                      const display_settings& display) {
    std::vector<unsigned char> bytes;
    bytes.reserve(pixels.size() * 3);
    for (const auto& pixel : pixels) {
        color c = display.apply(pixel);
        bytes.push_back(to_byte(c.x()));
        bytes.push_back(to_byte(c.y()));
        bytes.push_back(to_byte(c.z()));
    }
    return stbi_write_png(path.c_str(), width, height, 3, bytes.data(), width * 3) != 0;
}

// Writes raw linear radiance as Radiance RGBE (.hdr).
inline bool write_hdr(const std::string& path, int width, int height, const std::vector<color>& pixels) {
    std::vector<float> floats;
    floats.reserve(pixels.size() * 3);
    for (const auto& pixel : pixels)
        for (int k = 0; k < 3; k++)
            floats.push_back(float(pixel[k]));
    return stbi_write_hdr(path.c_str(), width, height, 3, floats.data()) != 0;
}

// Writes raw linear radiance as a little-endian Portable Float Map (.pfm),
// stored bottom row first as the format requires.
inline bool write_pfm(const std::string& path, int width, int height, const std::vector<color>& pixels) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out << "PF\n" << width << ' ' << height << "\n-1.0\n";
    for (int j = height - 1; j >= 0; j--) {
        for (int i = 0; i < width; i++) {
            const color& pixel = pixels[size_t(j) * width + i];
            float rgb[3] = { float(pixel.x()), float(pixel.y()), float(pixel.z()) };
            out.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
        }
    }
    return bool(out);
}

inline bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool is_supported_image(const std::string& path) {
    return ends_with(path, ".png") || ends_with(path, ".ppm")
        || ends_with(path, ".hdr") || ends_with(path, ".pfm");
}

// Writes the image to path, choosing the format from the extension. .png and
// .ppm are display images (bloom, vignette, exposure, tonemap); .hdr and .pfm
// keep the raw radiance. An empty path writes PPM to stdout.
inline bool write_image(const std::string& path, int width, int height, const std::vector<color>& pixels,
                        const display_settings& display) {
    bool shown = path.empty() || ends_with(path, ".png") || ends_with(path, ".ppm");
    std::vector<color> post;
    if (shown && display.post.active())
        post = postprocess(pixels, width, height, display.post);
    const std::vector<color>& display_pixels = post.empty() ? pixels : post;

    bool ok = false;
    if (path.empty()) {
        write_ppm(std::cout, width, height, display_pixels, display);
        return true;
    } else if (ends_with(path, ".png")) {
        ok = write_png(path, width, height, display_pixels, display);
    } else if (ends_with(path, ".ppm")) {
        std::ofstream out(path);
        if (out)
            write_ppm(out, width, height, display_pixels, display);
        ok = bool(out);
    } else if (ends_with(path, ".hdr")) {
        ok = write_hdr(path, width, height, pixels);
    } else if (ends_with(path, ".pfm")) {
        ok = write_pfm(path, width, height, pixels);
    } else {
        std::cerr << "Error: unsupported output format '" << path << "' (use .png, .ppm, .hdr or .pfm)\n";
        return false;
    }
    if (!ok)
        std::cerr << "Error: cannot write '" << path << "'\n";
    return ok;
}

#endif
