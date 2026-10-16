// White furnace test: a sphere that neither absorbs nor emits, inside a
// uniform white environment, must be invisible. Every pixel should come out
// exactly as bright as the background. Darker pixels mean the integrator
// loses energy; brighter ones mean it creates energy.

#include <camera.h>
#include <hittable_list.h>
#include <material.h>
#include <sphere.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

static bool furnace(const std::string& name, std::shared_ptr<material> mat, double tolerance) {
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

    double worst = 0, sum = 0;
    for (const auto& p : pixels) {
        for (int k = 0; k < 3; k++) {
            worst = std::fmax(worst, std::fabs(p[k] - 1));
            sum += p[k];
        }
    }
    double mean = sum / (3.0 * pixels.size());

    bool ok = std::fabs(mean - 1) < tolerance && worst < 4 * tolerance;
    std::cout << (ok ? "PASS " : "FAIL ") << name << ": mean " << mean
              << ", worst pixel off by " << worst << '\n';
    return ok;
}

int main() {
    std::clog.setstate(std::ios::failbit);  // Silence render progress

    bool ok = true;
    ok &= furnace("white lambertian", std::make_shared<lambertian>(color(1, 1, 1)), 1e-6);
    ok &= furnace("perfect mirror", std::make_shared<metal>(color(1, 1, 1), 0.0), 1e-6);
    ok &= furnace("clear glass", std::make_shared<dielectric>(1.5), 0.02);
    return ok ? 0 : 1;
}
