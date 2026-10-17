#ifndef ENVIRONMENT_H
#define ENVIRONMENT_H

#include "color3.h"
#include "hittable.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <stb_image.h>

// Light arriving from infinitely far away, stored as an equirectangular
// (latitude-longitude) HDR image: column = azimuth, row = angle from +y.
// Looking down -z sees the middle of the image; "rotation" turns the sky
// about the y axis.
//
// It is a hittable only so it can sit in the scene's lights list: hit() never
// succeeds (rays that miss everything see it through camera::background_color),
// but pdf_value() and random() let next event estimation aim shadow rays at
// the bright parts of the sky.
//
// Sampling draws a pixel with probability proportional to its luminance times
// sin(theta), the solid angle its row covers, using a 2D table: a marginal
// CDF picks the row, then that row's conditional CDF picks the column.
class environment_light : public hittable{
    public:
        // Loads an .hdr (or any image stb can read); check loaded() afterwards.
        environment_light(const std::string& path, double intensity = 1, double rotation_degrees = 0)
            : intensity(intensity), rotation(rotation_degrees / 360)
        {
            float* data = stbi_loadf(path.c_str(), &width, &height, nullptr, 3);
            if (!data)
                return;
            pixels.resize(size_t(width) * height);
            for (size_t k = 0; k < pixels.size(); k++)
                pixels[k] = color(data[3*k], data[3*k + 1], data[3*k + 2]);
            stbi_image_free(data);
            build_distribution();
        }

        // From pixels already in memory, row by row from the top (+y).
        environment_light(int width, int height, std::vector<color> pixels,
                          double intensity = 1, double rotation_degrees = 0)
            : width(width), height(height), pixels(std::move(pixels)),
              intensity(intensity), rotation(rotation_degrees / 360)
        {
            build_distribution();
        }

        bool loaded() const { return !pixels.empty(); }

        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            (void)r; (void)ray_t; (void)rec;
            return false;
        }

        aabb bounding_box() const override { return aabb(); }

        // Radiance arriving along direction (from the sky towards the scene),
        // filtered bilinearly so the sky doesn't look blocky behind the scene.
        // Sampling still treats each pixel as constant; that only changes how
        // well the pdf matches, not the result, since BSDF sampling (MIS)
        // covers any direction the light pdf misses.
        color radiance(const vec3& direction) const {
            double u, theta;
            to_angles(direction, u, theta);
            auto x = u * width - 0.5;
            auto y = std::clamp(theta / pi * height - 0.5, 0.0, height - 1.0);
            auto x0 = std::floor(x), y0 = std::floor(y);
            auto fx = x - x0, fy = y - y0;
            int i0 = (int(x0) + width) % width, i1 = (i0 + 1) % width;  // Wraps around in azimuth
            int j0 = int(y0), j1 = std::min(j0 + 1, height - 1);
            auto at = [&](int i, int j) { return pixels[size_t(j) * width + i]; };
            auto top = (1 - fx) * at(i0, j0) + fx * at(i1, j0);
            auto bottom = (1 - fx) * at(i0, j1) + fx * at(i1, j1);
            return intensity * ((1 - fy) * top + fy * bottom);
        }

        // A pixel covers du dv = 1 / (width height) of the image and a solid
        // angle of 2 pi^2 sin(theta) du dv, so the solid angle density is
        // p(u, v) / (2 pi^2 sin(theta)).
        double pdf_value(const point3& origin, const vec3& direction) const override {
            (void)origin;
            int i, j;
            double sin_theta;
            to_pixel(direction, i, j, sin_theta);
            if (sin_theta <= 0)
                return 0;
            return pixel_pdf(i, j) / (2 * pi * pi * sin_theta);
        }

        vec3 random(const point3& origin) const override {
            (void)origin;
            // Pick a row from the marginal CDF, then a column from that row's
            // CDF; the leftover fraction places the point inside the pixel, so
            // the density is constant over each pixel in (u, v).
            double y = sample_cdf(marginal_cdf.data(), height, random_double());
            int j = std::min(int(y), height - 1);
            double x = sample_cdf(&conditional_cdf[size_t(j) * (width + 1)], width, random_double());

            auto theta = pi * y / height;
            auto phi = 2 * pi * (x / width - 0.5 - rotation);
            auto sin_theta = std::sin(theta);
            return vec3(sin_theta * std::sin(phi), std::cos(theta), -sin_theta * std::cos(phi));
        }

    private:
        int width = 0, height = 0;
        std::vector<color> pixels;
        double intensity;
        double rotation;  // In turns

        std::vector<double> conditional_cdf;  // height rows of width + 1 entries
        std::vector<double> marginal_cdf;     // height + 1 entries
        std::vector<double> row_weight;       // Unnormalized weight of each row
        double total_weight = 0;

        // Pixel weight before normalization: brightness times the solid angle
        // of its row (rows near the poles are squeezed into tiny solid angles).
        double weight(int i, int j) const {
            auto sin_theta = std::sin(pi * (j + 0.5) / height);
            return luminance(pixels[size_t(j) * width + i]) * sin_theta;
        }

        void build_distribution() {
            if (pixels.empty())
                return;

            conditional_cdf.assign(size_t(height) * (width + 1), 0.0);
            marginal_cdf.assign(height + 1, 0.0);
            row_weight.assign(height, 0.0);

            for (int j = 0; j < height; j++) {
                double* cdf = &conditional_cdf[size_t(j) * (width + 1)];
                for (int i = 0; i < width; i++)
                    cdf[i + 1] = cdf[i] + std::fmax(weight(i, j), 0.0);
                row_weight[j] = cdf[width];
                for (int i = 1; i <= width; i++)
                    cdf[i] = row_weight[j] > 0 ? cdf[i] / row_weight[j] : double(i) / width;
                marginal_cdf[j + 1] = marginal_cdf[j] + row_weight[j];
            }
            total_weight = marginal_cdf[height];

            if (total_weight <= 0) {
                // A black sky: fall back to uniform rows weighted by sin(theta)
                for (int j = 0; j < height; j++) {
                    row_weight[j] = std::sin(pi * (j + 0.5) / height);
                    marginal_cdf[j + 1] = marginal_cdf[j] + row_weight[j];
                }
                total_weight = marginal_cdf[height];
            }
            for (int j = 1; j <= height; j++)
                marginal_cdf[j] /= total_weight;
        }

        // Inverts a piecewise-constant CDF of n cells: returns a continuous
        // position in [0, n), the cell index plus the fraction inside it.
        static double sample_cdf(const double* cdf, int n, double xi) {
            int k = int(std::upper_bound(cdf, cdf + n + 1, xi) - cdf) - 1;
            k = std::clamp(k, 0, n - 1);
            auto span = cdf[k + 1] - cdf[k];
            auto fraction = span > 0 ? (xi - cdf[k]) / span : 0.5;
            return k + std::clamp(fraction, 0.0, 1.0 - 1e-9);
        }

        // Density over (u, v) in [0,1]^2 of the pixel i, j being sampled:
        // P(row) P(column | row) times the width * height pixels per unit area.
        double pixel_pdf(int i, int j) const {
            double row = row_weight[j] / total_weight;
            const double* cdf = &conditional_cdf[size_t(j) * (width + 1)];
            double column = cdf[i + 1] - cdf[i];
            return row * column * width * height;
        }

        // Image coordinate u in [0, 1) (azimuth) and angle theta from +y.
        void to_angles(const vec3& direction, double& u, double& theta) const {
            vec3 d = unit_vector(direction);
            theta = std::acos(std::clamp(d.y(), -1.0, 1.0));
            u = 0.5 + std::atan2(d.x(), -d.z()) / (2 * pi) + rotation;
            u -= std::floor(u);
        }

        // The pixel a direction falls in.
        void to_pixel(const vec3& direction, int& i, int& j, double& sin_theta) const {
            double u, theta;
            to_angles(direction, u, theta);
            sin_theta = std::sin(theta);
            i = std::clamp(int(u * width), 0, width - 1);
            j = std::clamp(int(theta / pi * height), 0, height - 1);
        }
};

#endif
