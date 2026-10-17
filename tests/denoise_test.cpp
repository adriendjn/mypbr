// Checks the denoiser on synthetic frames with known answers: it must
// average noise away, keep the mean, keep albedo and normal edges sharp, and
// remove a lone firefly without touching a converged highlight.

#include <denoiser.h>

#include <cmath>
#include <iostream>
#include <random>
#include <string>

static const int width = 64;
static const int height = 64;

static bool check(const std::string& name, bool ok, double got) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << ": " << got << '\n';
    return ok;
}

// A frame whose pixel i, j has the given albedo, normal and noise-free
// radiance, plus Gaussian noise of the given relative deviation (as from
// averaging few samples), with the matching variance.
template <typename F>
static frame make_frame(double noise, unsigned seed, const F& pixel) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    frame f;
    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            color albedo, normal, radiance;
            pixel(i, j, albedo, normal, radiance);
            double sigma = noise * luminance(radiance);
            f.radiance.push_back(radiance * (1 + noise * gauss(rng)));
            f.albedo.push_back(albedo);
            f.normal.push_back(normal);
            f.variance.push_back(sigma * sigma);
        }
    }
    return f;
}

static double rms_error(const std::vector<color>& got, const std::vector<color>& want) {
    double sum = 0;
    for (size_t k = 0; k < got.size(); k++)
        sum += (got[k] - want[k]).length_squared();
    return std::sqrt(sum / got.size());
}

int main() {
    bool ok = true;
    denoiser filter;
    const vec3 up(0, 0, 1), side(1, 0, 0);

    // Flat grey wall: the noise should mostly vanish, the mean stay put.
    {
        auto f = make_frame(0.3, 1, [](int, int, color& a, color& n, color& r) {
            a = color(0.5, 0.5, 0.5); n = vec3(0, 0, 1); r = color(0.5, 0.5, 0.5);
        });
        std::vector<color> truth(f.radiance.size(), color(0.5, 0.5, 0.5));
        auto out = filter.run(f, width, height);
        double before = rms_error(f.radiance, truth), after = rms_error(out, truth);
        ok &= check("flat wall noise reduced 5x (ratio)", after < before / 5, before / after);
        double mean = 0;
        for (const auto& c : out)
            mean += luminance(c) / out.size();
        ok &= check("flat wall mean kept", std::fabs(mean - 0.5) < 0.01, mean);
    }

    // Texture edge: same lighting, albedo 0.2 on the left and 0.8 on the
    // right. Neither side may bleed into the other.
    {
        auto texture = [](int i, int, color& a, color& n, color& r) {
            double v = i < width / 2 ? 0.2 : 0.8;
            a = color(v, v, v); n = vec3(0, 0, 1); r = a;
        };
        auto f = make_frame(0.3, 2, texture);
        auto clean = make_frame(0.0, 2, texture);
        auto out = filter.run(f, width, height);
        double worst = 0;
        for (int j = 0; j < height; j++)
            for (int i = width / 2 - 1; i <= width / 2; i++) {
                size_t k = size_t(j) * width + i;
                worst = std::fmax(worst, std::fabs(luminance(out[k]) - luminance(clean.radiance[k])));
            }
        ok &= check("albedo edge kept (worst error next to it)", worst < 0.06, worst);
    }

    // Geometric edge: same albedo, a wall facing the light (bright) meets
    // one facing away (dark); the normals tell them apart.
    {
        auto corner = [&](int i, int, color& a, color& n, color& r) {
            a = color(0.7, 0.7, 0.7);
            n = i < width / 2 ? up : side;
            r = i < width / 2 ? color(1, 1, 1) : color(0.1, 0.1, 0.1);
        };
        auto f = make_frame(0.3, 3, corner);
        auto clean = make_frame(0.0, 3, corner);
        auto out = filter.run(f, width, height);
        size_t k = size_t(height / 2) * width + width / 2;  // First dark pixel
        double got = luminance(out[k]);
        ok &= check("normal edge kept (dark side next to it)", std::fabs(got - 0.1) < 0.03, got);
    }

    // A converged image with one firefly (one sample of 32 found the sun:
    // its deviation is as large as its mean) and one real one-pixel highlight
    // (low deviation). The firefly must go, the highlight stay.
    {
        auto f = make_frame(0.0, 4, [](int, int, color& a, color& n, color& r) {
            a = color(0.5, 0.5, 0.5); n = vec3(0, 0, 1); r = color(0.5, 0.5, 0.5);
        });
        size_t firefly = size_t(20) * width + 20, highlight = size_t(40) * width + 40;
        f.radiance[firefly] = color(50, 50, 50);
        f.variance[firefly] = 50 * 50;
        f.radiance[highlight] = color(50, 50, 50);
        f.variance[highlight] = 5 * 5;
        auto out = filter.run(f, width, height);
        ok &= check("firefly removed", luminance(out[firefly]) < 1, luminance(out[firefly]));
        ok &= check("highlight kept", luminance(out[highlight]) > 25, luminance(out[highlight]));
    }

    return ok ? 0 : 1;
}
