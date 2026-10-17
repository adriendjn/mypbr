// White furnace test: a sphere that neither absorbs nor emits, inside a
// uniform white environment, must be invisible. Every pixel should come out
// exactly as bright as the background. Darker pixels mean the integrator
// loses energy; brighter ones mean it creates energy.
//
// Rough glass and the dielectric principled material lose some energy by
// design (single-scattering GGX ignores light bouncing between microfacets),
// so for those the test only checks that energy stays bounded: never brighter
// than the background. White rough metals get the Kulla-Conty compensation
// lobe, so they must reflect everything again.

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

// Mean value of the sphere's own pixels, without the background around it.
// Rendering the same sphere black measures how much of each pixel it covers:
// the black image is 1 - coverage.
static double object_energy(std::shared_ptr<material> mat) {
    double mean, black_mean, worst;
    render_furnace(furnace_sphere(mat), mean, worst);
    render_furnace(furnace_sphere(std::make_shared<lambertian>(color(0, 0, 0))), black_mean, worst);
    return (mean - black_mean) / (1 - black_mean);
}

// A white conductor must reflect all light that reaches its pixels.
static bool energy_conserving(const std::string& name, std::shared_ptr<material> mat, double tolerance) {
    double energy = object_energy(mat);
    bool ok = std::fabs(energy - 1) < tolerance;
    std::cout << (ok ? "PASS " : "FAIL ") << name << ": sphere pixels " << energy
              << " (tolerance " << tolerance << ")\n";
    return ok;
}

// Directional albedo, the integral of eval over all directions for a view at
// cos_o, estimated with the material's own sampling: E[eval / pdf].
static double directional_albedo(const material& mat, double cos_o) {
    hit_record rec{};
    rec.p = point3(0, 0, 0);
    rec.normal = vec3(0, 0, 1);
    rec.front_face = true;
    vec3 wo(std::sqrt(1 - cos_o * cos_o), 0, cos_o);
    ray r_in(wo, -wo);

    const int n = 200000;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        scatter_record srec;
        if (!mat.scatter(r_in, rec, srec))
            continue;
        vec3 d = srec.pdf_ptr->generate();
        double p = d.length_squared() > 0 ? srec.pdf_ptr->value(d) : 0;
        if (p > 0) {
            color f = mat.eval(r_in, rec, d);
            sum += (f.x() + f.y() + f.z()) / (3 * p);
        }
    }
    return sum / n;
}

static bool albedo_one(const std::string& name, const material& mat, double cos_o, double tolerance) {
    double albedo = directional_albedo(mat, cos_o);
    bool ok = std::fabs(albedo - 1) < tolerance;
    std::cout << (ok ? "PASS " : "FAIL ") << name << ", cos_o " << cos_o << ": directional albedo "
              << albedo << " (tolerance " << tolerance << ")\n";
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

    // Multiple-scattering compensation: white rough conductors lose nothing.
    // The sphere pixels average over every view angle; the directional
    // albedo checks single angles, grazing ones included, with less noise.
    for (double roughness : {0.5, 1.0}) {
        auto r = std::to_string(roughness).substr(0, 3);
        auto white_metal = std::make_shared<metal>(color(1, 1, 1), roughness);
        ok &= energy_conserving("compensated GGX metal, roughness " + r, white_metal, 0.02);
        ok &= energy_conserving("compensated principled metallic, roughness " + r,
                                std::make_shared<principled>(white, 1.0, roughness, 1.5), 0.02);
        for (double cos_o : {1.0, 0.5, 0.1})
            ok &= albedo_one("compensated GGX metal, roughness " + r, *white_metal, cos_o, 0.01);
    }
    return ok ? 0 : 1;
}
