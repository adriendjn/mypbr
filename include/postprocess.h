#ifndef POSTPROCESS_H
#define POSTPROCESS_H

#include "color3.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Image effects applied to display images (.png, .ppm) before tonemapping.
// They imitate a camera rather than the scene, so the raw .hdr and .pfm
// outputs leave them out.
class post_settings{
    public:
        double bloom = 0;              // Strength: how much of the glow is added (0 = off)
        double bloom_threshold = 1;    // Radiance (luminance) above which pixels glow
        double bloom_radius = 0.04;    // Reach of the glow, as a fraction of the image width
        double vignette = 0;           // Darkening of the corners (0 = off)

        bool active() const { return bloom > 0 || vignette > 0; }
};

namespace post_detail {

// An image with its size, for the bloom pyramid.
class image{
    public:
        int w = 0, h = 0;
        std::vector<color> px;

        color at(int i, int j) const {
            i = std::clamp(i, 0, w - 1);
            j = std::clamp(j, 0, h - 1);
            return px[size_t(j) * w + i];
        }

        // Bilinear lookup at continuous pixel coordinates (centers at k + 0.5)
        color sample(double x, double y) const {
            x -= 0.5; y -= 0.5;
            int i = int(std::floor(x)), j = int(std::floor(y));
            double fx = x - i, fy = y - j;
            return (1 - fy) * ((1 - fx) * at(i, j) + fx * at(i + 1, j))
                 + fy * ((1 - fx) * at(i, j + 1) + fx * at(i + 1, j + 1));
        }
};

// Half the size, each pixel the average of the 2x2 below it
inline image downsample(const image& in) {
    image out;
    out.w = std::max(1, in.w / 2);
    out.h = std::max(1, in.h / 2);
    out.px.resize(size_t(out.w) * out.h);
    for (int j = 0; j < out.h; j++)
        for (int i = 0; i < out.w; i++)
            out.px[size_t(j) * out.w + i] = 0.25 * (in.at(2*i, 2*j) + in.at(2*i + 1, 2*j)
                                                  + in.at(2*i, 2*j + 1) + in.at(2*i + 1, 2*j + 1));
    return out;
}

// Separable Gaussian blur, sigma in pixels
inline image blur(const image& in, double sigma) {
    int r = int(std::ceil(3 * sigma));
    std::vector<double> kernel(2 * r + 1);
    double sum = 0;
    for (int k = -r; k <= r; k++)
        sum += kernel[k + r] = std::exp(-0.5 * k * k / (sigma * sigma));
    for (auto& k : kernel)
        k /= sum;

    image tmp = in, out = in;
    for (int j = 0; j < in.h; j++)
        for (int i = 0; i < in.w; i++) {
            color c(0, 0, 0);
            for (int k = -r; k <= r; k++)
                c += kernel[k + r] * in.at(i + k, j);
            tmp.px[size_t(j) * in.w + i] = c;
        }
    for (int j = 0; j < in.h; j++)
        for (int i = 0; i < in.w; i++) {
            color c(0, 0, 0);
            for (int k = -r; k <= r; k++)
                c += kernel[k + r] * tmp.at(i, j + k);
            out.px[size_t(j) * in.w + i] = c;
        }
    return out;
}

}  // namespace post_detail

// Returns pixels (linear radiance, row by row) with bloom and vignetting.
//
// Bloom: a real lens and the eye scatter a little of every pixel's light
// onto its neighbours, with a long, faint tail. Only very bright pixels
// have enough light for that to show, so they make the glow around lamps
// and the sun. The bright part of the image (radiance above the threshold)
// is blurred at several scales, each twice as wide as the last, using a
// pyramid of half-size images so wide blurs stay cheap; their average is a
// sharp core with a wide tail, added back with the given strength. It works
// on radiance, before tonemapping, because the scattering in the lens does.
//
// Vignetting: lenses pass less light towards the corners of the frame,
// roughly as 1 / (1 + k r^2)^2 with r the distance from the center (1 at
// the corners).
inline std::vector<color> postprocess(const std::vector<color>& pixels, int width, int height,
                                      const post_settings& post) {
    std::vector<color> out = pixels;
    if (post.bloom > 0) {
        using namespace post_detail;
        image bright;
        bright.w = width;
        bright.h = height;
        bright.px.resize(pixels.size());
        for (size_t k = 0; k < pixels.size(); k++) {
            // Keep what exceeds the threshold, in proportion, so colors stay
            double l = luminance(pixels[k]);
            bright.px[k] = (std::isfinite(l) && l > post.bloom_threshold)
                         ? pixels[k] * ((l - post.bloom_threshold) / l) : color(0, 0, 0);
        }

        // Levels until the blur reaches the requested radius
        double reach = std::fmax(post.bloom_radius * width, 1.0);
        int levels = std::clamp(int(std::ceil(std::log2(reach))), 1, 10);
        std::vector<image> blurred;
        image level = bright;
        for (int k = 0; k < levels && level.w > 1 && level.h > 1; k++) {
            level = downsample(level);
            blurred.push_back(blur(level, 1.5));
        }
        for (int j = 0; j < height; j++)
            for (int i = 0; i < width; i++) {
                color glow(0, 0, 0);
                for (const auto& b : blurred) {
                    double sx = double(b.w) / width, sy = double(b.h) / height;
                    glow += b.sample((i + 0.5) * sx, (j + 0.5) * sy);
                }
                if (!blurred.empty())
                    out[size_t(j) * width + i] += post.bloom / blurred.size() * glow;
            }
    }
    if (post.vignette > 0) {
        double cx = 0.5 * width, cy = 0.5 * height;
        double corner2 = cx * cx + cy * cy;
        for (int j = 0; j < height; j++)
            for (int i = 0; i < width; i++) {
                double dx = i + 0.5 - cx, dy = j + 0.5 - cy;
                double f = 1 + post.vignette * (dx * dx + dy * dy) / corner2;
                out[size_t(j) * width + i] = out[size_t(j) * width + i] / (f * f);
            }
    }
    return out;
}

#endif
