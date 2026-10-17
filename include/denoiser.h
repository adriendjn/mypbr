#ifndef DENOISER_H
#define DENOISER_H

#include "color3.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

// What a render produces per pixel, row by row from the top left. Only
// radiance is always filled; the rest are the denoiser's guides, filled when
// the camera's collect_features is on.
class frame{
    public:
        std::vector<color>  radiance;  // Mean of the samples: the image
        std::vector<color>  albedo;    // Mean reflectance at the first non-specular hit
        std::vector<color>  normal;    // Mean normal there (shorter than 1 where it varies)
        std::vector<double> variance;  // Variance of the mean luminance (shrinks as 1/spp)
};

// Edge-avoiding a-trous wavelet filter (Dammertz et al. 2010), with the
// variance-guided luminance weight of SVGF (Schied et al. 2017), applied to
// a single frame.
//
// Every pixel is averaged with its neighbours, but a neighbour only counts
// if it shows the same surface: similar normal, similar albedo, and a
// luminance within a few standard deviations of the pixel's own noise. Noise
// is averaged away; edges, which differ in one of those, survive.
//
// Texture detail would be blurred, so the filter runs on radiance divided by
// albedo (the lighting alone, which is smooth across a texture) and the
// albedo is multiplied back afterwards.
//
// Each iteration is a 5x5 B3-spline kernel whose taps are spread 2^i pixels
// apart, so five iterations cover a 61x61 footprint at the cost of 125 taps.
class denoiser{
    public:
        int    iterations = 5;
        double sigma_luminance = 4;    // In standard deviations of the noise
        double sigma_normal    = 0.1;  // Distance between normals (0.1 is about 6 degrees)
        double sigma_albedo    = 0.1;
        int    threads = 0;            // 0 uses every hardware thread

        std::vector<color> run(const frame& f, int width, int height) const {
            size_t n = size_t(width) * height;
            std::vector<color> lighting(n);
            std::vector<double> var(n);
            for (size_t k = 0; k < n; k++) {
                color a = f.albedo[k] + color(albedo_epsilon, albedo_epsilon, albedo_epsilon);
                const color& c = f.radiance[k];
                lighting[k] = color(c.x() / a.x(), c.y() / a.y(), c.z() / a.z());
                double la = luminance(a);
                var[k] = f.variance[k] / (la * la);
            }
            remove_fireflies(lighting, var, width, height);

            std::vector<color> next_lighting(n);
            std::vector<double> next_var(n);
            for (int it = 0; it < iterations; it++) {
                auto blurred_var = blur3x3(var, width, height);
                parallel_rows(height, [&](int j) {
                    for (int i = 0; i < width; i++)
                        filter_pixel(i, j, 1 << it, width, height, f, lighting, var, blurred_var,
                                     next_lighting, next_var);
                });
                std::swap(lighting, next_lighting);
                std::swap(var, next_var);
            }

            std::vector<color> out(n);
            for (size_t k = 0; k < n; k++)
                out[k] = lighting[k] * (f.albedo[k] + color(albedo_epsilon, albedo_epsilon, albedo_epsilon));
            return out;
        }

    private:
        // Keeps black surfaces from dividing by zero; multiplied back exactly
        static constexpr double albedo_epsilon = 1e-3;

        // A firefly is one rare, very bright sample (a caustic path that found
        // the sun) that dominates its pixel's mean. Its noise estimate is as
        // large as the pixel itself, so the luminance test would accept every
        // neighbour for it while they all reject it, leaving a bright dot.
        // Pixels brighter than all eight neighbours whose standard deviation
        // is over half their value are clamped to the brightest neighbour.
        // A real highlight, even one pixel wide, comes from many samples and
        // has a much lower relative deviation, so it is kept. This removes
        // energy, which is the bias traded for a clean image.
        static void remove_fireflies(std::vector<color>& lighting, std::vector<double>& var,
                                     int width, int height) {
            std::vector<color> source = lighting;
            for (int j = 1; j < height - 1; j++) {
                for (int i = 1; i < width - 1; i++) {
                    size_t p = size_t(j) * width + i;
                    double lum = luminance(source[p]);
                    if (lum <= 0 || std::sqrt(var[p]) < 0.5 * lum)
                        continue;
                    double brightest = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                            if (dx != 0 || dy != 0)
                                brightest = std::fmax(brightest, luminance(source[p + dy * width + dx]));
                    if (lum > brightest) {
                        double scale = brightest / lum;
                        lighting[p] = scale * source[p];
                        var[p] *= scale * scale;
                    }
                }
            }
        }

        void filter_pixel(int i, int j, int step, int width, int height, const frame& f,
                          const std::vector<color>& lighting, const std::vector<double>& var,
                          const std::vector<double>& blurred_var,
                          std::vector<color>& out, std::vector<double>& out_var) const {
            static const double kernel[5] = {1.0/16, 1.0/4, 3.0/8, 1.0/4, 1.0/16};

            size_t p = size_t(j) * width + i;
            double lum_p = luminance(lighting[p]);
            // The noise's standard deviation, from a slightly smoothed
            // variance so one lucky or unlucky pixel doesn't set it alone
            double lum_scale = sigma_luminance * std::sqrt(std::fmax(blurred_var[p], 0.0)) + 1e-10;
            double inv_normal = 1 / (sigma_normal * sigma_normal);
            double inv_albedo = 1 / (sigma_albedo * sigma_albedo);

            color sum(0, 0, 0);
            double weight_sum = 0;
            double var_sum = 0;
            for (int dy = -2; dy <= 2; dy++) {
                int y = j + dy * step;
                if (y < 0 || y >= height)
                    continue;
                for (int dx = -2; dx <= 2; dx++) {
                    int x = i + dx * step;
                    if (x < 0 || x >= width)
                        continue;
                    size_t q = size_t(y) * width + x;

                    double w_normal = std::exp(-(f.normal[p] - f.normal[q]).length_squared() * inv_normal);
                    double w_albedo = std::exp(-(f.albedo[p] - f.albedo[q]).length_squared() * inv_albedo);
                    double w_lum = std::exp(-std::fabs(lum_p - luminance(lighting[q])) / lum_scale);
                    double w = kernel[dx + 2] * kernel[dy + 2] * w_normal * w_albedo * w_lum;

                    sum += w * lighting[q];
                    weight_sum += w;
                    var_sum += w * w * var[q];
                }
            }
            // The center tap has weight kernel^2 > 0, so weight_sum > 0.
            // A weighted mean of independent values has variance
            // sum(w^2 var) / (sum w)^2; the next iteration's luminance test
            // then compares against the noise that is left.
            out[p] = sum / weight_sum;
            out_var[p] = var_sum / (weight_sum * weight_sum);
        }

        static std::vector<double> blur3x3(const std::vector<double>& v, int width, int height) {
            static const double kernel[3] = {0.25, 0.5, 0.25};
            std::vector<double> out(v.size());
            for (int j = 0; j < height; j++) {
                for (int i = 0; i < width; i++) {
                    double sum = 0, weight = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            int x = i + dx, y = j + dy;
                            if (x < 0 || x >= width || y < 0 || y >= height)
                                continue;
                            double w = kernel[dx + 1] * kernel[dy + 1];
                            sum += w * v[size_t(y) * width + x];
                            weight += w;
                        }
                    }
                    out[size_t(j) * width + i] = sum / weight;
                }
            }
            return out;
        }

        // Runs row(j) for every row, split across worker threads. Each row
        // only writes its own pixels.
        template <typename F>
        void parallel_rows(int height, const F& row) const {
            int count = threads > 0 ? threads : int(std::thread::hardware_concurrency());
            count = std::clamp(count, 1, height);
            std::vector<std::thread> pool;
            for (int t = 1; t < count; t++)
                pool.emplace_back([&, t]() { for (int j = t; j < height; j += count) row(j); });
            for (int j = 0; j < height; j += count)
                row(j);
            for (auto& th : pool)
                th.join();
        }
};

#endif
