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
#include <medium.h>
#include <sphere.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

// Renders the sphere in the furnace; returns the mean pixel value and the
// largest deviation of any pixel channel from 1.
//
// With use_environment, the white background is a constant environment map in
// the lights, so the sphere is lit through next event estimation + MIS as well.
static void render_furnace(std::shared_ptr<hittable> object, double& mean, double& worst,
                           bool use_environment = false) {
    hittable_list world, lights;
    world.add(object);

    camera cam;
    cam.image_width = 32;
    cam.aspect_ratio = 1;
    cam.samples_per_pixel = use_environment ? 256 : 64;  // Light sampling adds noise
    cam.max_depth = 100;
    cam.use_sky = false;
    cam.background = color(1, 1, 1);
    if (use_environment) {
        cam.environment = std::make_shared<environment_light>(
            16, 8, std::vector<color>(16 * 8, color(1, 1, 1)));
        lights.add(cam.environment);
    }

    auto pixels = cam.render(world, lights).radiance;

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

static std::shared_ptr<hittable> furnace_sphere(std::shared_ptr<material> mat) {
    return std::make_shared<sphere>(point3(0, 0, -1), 0.5, mat);
}

// Energy conserving: the object must vanish.
static bool furnace(const std::string& name, std::shared_ptr<hittable> object, double tolerance,
                    bool use_environment = false) {
    double mean, worst;
    render_furnace(object, mean, worst, use_environment);
    bool ok = std::fabs(mean - 1) < tolerance && worst < 4 * tolerance;
    std::cout << (ok ? "PASS " : "FAIL ") << name << ": mean " << mean
              << ", worst pixel off by " << worst << '\n';
    return ok;
}

// Energy bounded: the image may be darker than the background where the
// sphere is, but never brighter on average.
static bool bounded(const std::string& name, std::shared_ptr<material> mat) {
    double mean, worst;
    render_furnace(furnace_sphere(mat), mean, worst);
    bool ok = mean <= 1.005 && mean > 0.5;
    std::cout << (ok ? "PASS " : "FAIL ") << name << ": mean " << mean << " (bounded)\n";
    return ok;
}

int main() {
    std::clog.setstate(std::ios::failbit);  // Silence render progress

    bool ok = true;
    ok &= furnace("white lambertian", furnace_sphere(std::make_shared<lambertian>(color(1, 1, 1))), 1e-6);
    ok &= furnace("perfect mirror", furnace_sphere(std::make_shared<metal>(color(1, 1, 1), 0.0)), 1e-6);
    ok &= furnace("clear glass", furnace_sphere(std::make_shared<dielectric>(1.5)), 0.02);
    // Light sampling makes this one noisy (not exact like pure BSDF sampling
    // of a constant sky), so the tolerance is looser
    ok &= furnace("white lambertian, environment map",
                  furnace_sphere(std::make_shared<lambertian>(color(1, 1, 1))), 0.02, true);

    // A non-absorbing medium only redirects light, so it must vanish too. Paths
    // scatter many times inside it, so Russian roulette adds more per-pixel noise
    // than for one surface bounce: hence the looser tolerance.
    for (double g : {0.0, 0.7}) {
        auto boundary = std::make_shared<sphere>(point3(0, 0, -1), 0.5, nullptr);
        auto phase = std::make_shared<phase_material>(color(1, 1, 1), g);
        ok &= furnace("white medium, g " + std::to_string(g).substr(0, 3),
                      std::make_shared<constant_medium>(boundary, 2.0, phase), 0.03);
    }

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

    // Roughness and metallic from textures: a checkerboard of smooth and rough,
    // dielectric and metal patches, looked up per hit
    auto patches = [](double a, double b) {
        return std::make_shared<checker_texture>(0.2, std::make_shared<solid_color>(color(a, a, a)),
                                                 std::make_shared<solid_color>(color(b, b, b)));
    };
    ok &= bounded("principled, textured roughness and metallic",
                  std::make_shared<principled>(white, patches(0, 1), patches(0.1, 0.9), 1.5));

    return ok ? 0 : 1;
}
