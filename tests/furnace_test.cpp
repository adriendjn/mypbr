// White furnace test: a sphere that neither absorbs nor emits, inside a
// uniform white environment, must be invisible. Every pixel should come out
// exactly as bright as the background. Darker pixels mean the integrator
// loses energy; brighter ones mean it creates energy.
//
// Rough microfacet materials lose some energy by design (single-scattering
// GGX ignores light bouncing between microfacets), so for those the test
// only checks that energy stays bounded: never brighter than the background.

#include <camera.h>
#include <hittable_list.h>
#include <material.h>
#include <sphere.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

// Renders the sphere in the furnace; returns the mean pixel value and the
// largest deviation of any pixel channel from 1.
static void render_furnace(std::shared_ptr<material> mat, double& mean, double& worst) {
    hittable_list world, lights;
    world.add(std::make_shared<sphere>(point3(0, 0, -1), 0.5, mat));

    camera cam;
    cam.image_width = 32;
    cam.aspect_ratio = 1;
    cam.samples_per_pixel = 64;
    cam.max_depth = 100;
    cam.use_sky = false;
    cam.background = color(1, 1, 1);

    auto pixels = cam.render(world, lights);

    worst = 0;
    double sum = 0;
    for (const auto& p : pixels) {
        for (int k = 0; k < 3; k++) {
            worst = std::fmax(worst, std::fabs(p[k] - 1));
            sum += p[k];
        }
    }
    mean = sum / (3.0 * pixels.size());
}

// Energy conserving: the sphere must vanish.
static bool furnace(const std::string& name, std::shared_ptr<material> mat, double tolerance) {
    double mean, worst;
    render_furnace(mat, mean, worst);
    bool ok = std::fabs(mean - 1) < tolerance && worst < 4 * tolerance;
    std::cout << (ok ? "PASS " : "FAIL ") << name << ": mean " << mean
              << ", worst pixel off by " << worst << '\n';
    return ok;
}

// Energy bounded: the image may be darker than the background where the
// sphere is, but never brighter on average.
static bool bounded(const std::string& name, std::shared_ptr<material> mat) {
    double mean, worst;
    render_furnace(mat, mean, worst);
    bool ok = mean <= 1.005 && mean > 0.5;
    std::cout << (ok ? "PASS " : "FAIL ") << name << ": mean " << mean << " (bounded)\n";
    return ok;
}

int main() {
    std::clog.setstate(std::ios::failbit);  // Silence render progress

    bool ok = true;
    ok &= furnace("white lambertian", std::make_shared<lambertian>(color(1, 1, 1)), 1e-6);
    ok &= furnace("perfect mirror", std::make_shared<metal>(color(1, 1, 1), 0.0), 1e-6);
    ok &= furnace("clear glass", std::make_shared<dielectric>(1.5), 0.02);

    auto white = std::make_shared<solid_color>(color(1, 1, 1));
    for (double roughness : {0.1, 0.5, 1.0}) {
        auto r = std::to_string(roughness).substr(0, 3);
        ok &= bounded("GGX metal, roughness " + r, std::make_shared<metal>(color(1, 1, 1), roughness));
        ok &= bounded("rough glass, roughness " + r, std::make_shared<dielectric>(1.5, roughness));
        ok &= bounded("principled dielectric, roughness " + r,
                      std::make_shared<principled>(white, 0.0, roughness, 1.5));
        ok &= bounded("principled metallic, roughness " + r,
                      std::make_shared<principled>(white, 1.0, roughness, 1.5));
    }
    return ok ? 0 : 1;
}
