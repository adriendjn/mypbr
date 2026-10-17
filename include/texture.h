#ifndef TEXTURE_H
#define TEXTURE_H

#include "color3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <stb_image.h>

// A color that varies over a surface, looked up by texture coordinates
// (u, v) and/or the hit point p.
class texture{
    public:
        virtual ~texture() = default;

        virtual color value(double u, double v, const point3& p) const = 0;
};

class solid_color : public texture{
    public:
        solid_color(const color& albedo) : albedo(albedo) {}

        color value(double u, double v, const point3& p) const override {
            (void)u; (void)v; (void)p;
            return albedo;
        }

    private:
        color albedo;
};

// A 3D checkerboard of cubes of side scale, alternating between two textures.
class checker_texture : public texture{
    public:
        checker_texture(double scale, std::shared_ptr<texture> even, std::shared_ptr<texture> odd)
            : inv_scale(1.0 / scale), even(even), odd(odd) {}

        color value(double u, double v, const point3& p) const override {
            auto x = int(std::floor(inv_scale * p.x()));
            auto y = int(std::floor(inv_scale * p.y()));
            auto z = int(std::floor(inv_scale * p.z()));

            bool is_even = (x + y + z) % 2 == 0;
            return is_even ? even->value(u, v, p) : odd->value(u, v, p);
        }

    private:
        double inv_scale;
        std::shared_ptr<texture> even;
        std::shared_ptr<texture> odd;
};

// An image (PNG, JPEG, TGA, HDR, ...) mapped over the (u, v) unit square,
// with v = 0 at the bottom row. 8-bit images are assumed sRGB encoded and
// converted to linear values; HDR images are already linear. With linear set,
// 8-bit values are taken as they are (k / 255): for data rather than colors,
// such as normal maps or roughness maps, which no sRGB curve was applied to.
//
// Coordinates outside [0, 1] are clamped to the edge, or wrapped around with
// repeat set (tiled textures on meshes). alpha_value() reads the alpha
// channel, or the grey level of an image without one, for use as a mask.
class image_texture : public texture{
    public:
        // Loads the image; check loaded() afterwards.
        explicit image_texture(const std::string& path, bool repeat = false, bool linear = false)
            : repeat(repeat), linear(linear) {
            if (stbi_is_hdr(path.c_str())) {
                float* data = stbi_loadf(path.c_str(), &width, &height, nullptr, 3);
                if (!data)
                    return;
                hdr_pixels.assign(data, data + size_t(width) * height * 3);
                stbi_image_free(data);
            } else {
                // Kept as bytes, 4 per pixel, and decoded on lookup: a large
                // textured model would need gigabytes as doubles.
                int channels;
                unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
                if (!data)
                    return;
                ldr_pixels.assign(data, data + size_t(width) * height * 4);
                stbi_image_free(data);
                // Grey and grey+alpha images have 1 and 2 channels
                alpha_channel = channels == 2 || channels == 4;
                mask_offset = alpha_channel ? 3 : 0;
                for (size_t k = mask_offset; k < ldr_pixels.size(); k += 4)
                    if (ldr_pixels[k] < 255) {
                        opaque = false;
                        break;
                    }
                grey = channels <= 2;
                for (size_t k = 0; grey && k < ldr_pixels.size(); k += 4)
                    grey = ldr_pixels[k] == ldr_pixels[k + 1] && ldr_pixels[k] == ldr_pixels[k + 2];
            }
        }

        bool loaded() const { return width > 0 && height > 0; }

        // Whether the file has an alpha channel, and whether alpha_value() is
        // 1 everywhere (always the case for HDR images).
        bool has_alpha_channel() const { return alpha_channel; }
        bool fully_opaque() const { return opaque; }

        // Whether every pixel has equal red, green and blue: a height or
        // mask image rather than a color or normal map. False for HDR images.
        bool greyscale() const { return grey; }

        color value(double u, double v, const point3& p) const override {
            (void)p;
            size_t k = pixel_index(u, v);
            if (!hdr_pixels.empty())
                return color(hdr_pixels[3*k], hdr_pixels[3*k + 1], hdr_pixels[3*k + 2]);
            if (linear)
                return color(ldr_pixels[4*k], ldr_pixels[4*k + 1], ldr_pixels[4*k + 2]) / 255.0;
            const auto& table = srgb_table();
            return color(table[ldr_pixels[4*k]], table[ldr_pixels[4*k + 1]], table[ldr_pixels[4*k + 2]]);
        }

        // Coverage in [0, 1] at (u, v), 1 being opaque: the alpha channel, or
        // the grey level without one. Masks are stored linearly, not as sRGB.
        double alpha_value(double u, double v) const {
            if (opaque)
                return 1;
            return ldr_pixels[4 * pixel_index(u, v) + mask_offset] / 255.0;
        }

    private:
        int width = 0, height = 0;
        bool repeat;
        bool linear;  // 8-bit values are data, not sRGB encoded colors
        bool alpha_channel = false;
        bool opaque = true;
        bool grey = false;
        int mask_offset = 3;  // Byte of each pixel that alpha_value() reads
        std::vector<unsigned char> ldr_pixels;  // RGBA, sRGB encoded unless linear
        std::vector<float> hdr_pixels;          // RGB, linear

        // Nearest pixel to (u, v), as an index into the pixel arrays.
        size_t pixel_index(double u, double v) const {
            if (repeat) {
                u -= std::floor(u);
                v -= std::floor(v);
            } else {
                u = std::fmin(std::fmax(u, 0.0), 1.0);
                v = std::fmin(std::fmax(v, 0.0), 1.0);
            }
            v = 1.0 - v;  // Flip V to image coordinates

            auto i = std::clamp(int(u * width), 0, width - 1);
            auto j = std::clamp(int(v * height), 0, height - 1);
            return size_t(j) * width + i;
        }

        // Linear value of each 8-bit sRGB code, computed once.
        static const std::array<double, 256>& srgb_table() {
            static const std::array<double, 256> table = [] {
                std::array<double, 256> t{};
                for (int k = 0; k < 256; k++) {
                    double encoded = k / 255.0;
                    t[k] = encoded <= 0.04045 ? encoded / 12.92
                                              : std::pow((encoded + 0.055) / 1.055, 2.4);
                }
                return t;
            }();
            return table;
        }
};

#endif
