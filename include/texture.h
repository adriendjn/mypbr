#ifndef TEXTURE_H
#define TEXTURE_H

#include "color3.h"

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

// An image (PNG, JPEG, HDR, ...) mapped over the (u, v) unit square, with
// v = 0 at the bottom row. 8-bit images are assumed sRGB encoded and
// converted to linear values; HDR images are already linear.
class image_texture : public texture{
    public:
        // Loads the image; check loaded() afterwards.
        explicit image_texture(const std::string& path) {
            if (stbi_is_hdr(path.c_str())) {
                float* data = stbi_loadf(path.c_str(), &width, &height, nullptr, 3);
                if (!data)
                    return;
                pixels.resize(size_t(width) * height);
                for (size_t k = 0; k < pixels.size(); k++)
                    pixels[k] = color(data[3*k], data[3*k + 1], data[3*k + 2]);
                stbi_image_free(data);
            } else {
                unsigned char* data = stbi_load(path.c_str(), &width, &height, nullptr, 3);
                if (!data)
                    return;
                pixels.resize(size_t(width) * height);
                for (size_t k = 0; k < pixels.size(); k++)
                    pixels[k] = color(srgb_to_linear(data[3*k] / 255.0),
                                      srgb_to_linear(data[3*k + 1] / 255.0),
                                      srgb_to_linear(data[3*k + 2] / 255.0));
                stbi_image_free(data);
            }
        }

        bool loaded() const { return !pixels.empty(); }

        color value(double u, double v, const point3& p) const override {
            (void)p;
            // Clamp input texture coordinates to [0,1] x [1,0]
            u = std::fmin(std::fmax(u, 0.0), 1.0);
            v = 1.0 - std::fmin(std::fmax(v, 0.0), 1.0);  // Flip V to image coordinates

            auto i = std::min(int(u * width), width - 1);
            auto j = std::min(int(v * height), height - 1);
            return pixels[size_t(j) * width + i];
        }

    private:
        int width = 0, height = 0;
        std::vector<color> pixels;

        static double srgb_to_linear(double encoded) {
            if (encoded <= 0.04045)
                return encoded / 12.92;
            return std::pow((encoded + 0.055) / 1.055, 2.4);
        }
};

#endif
