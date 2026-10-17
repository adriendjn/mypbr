// Checks that each pdf integrates to one over the sphere of directions and
// that its value() matches the density of the directions generate() draws.

#include <environment.h>
#include <hittable_list.h>
#include <material.h>
#include <microfacet.h>
#include <pdf.h>
#include <quad.h>
#include <sphere.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

// Monte Carlo estimate of the integral of p over all directions.
static double integral(const pdf& p) {
    const int n = 1000000;
    sphere_pdf uniform;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        vec3 d = uniform.generate();
        sum += p.value(d) / uniform.value(d);
    }
    return sum / n;
}

// E[f(d) / p(d)] with d drawn from p equals the integral of f over the
// sphere; with f = 1 on a cap that p covers, it must equal the cap's solid
// angle. This catches a value() that disagrees with generate().
static double cap_solid_angle(const pdf& p, const vec3& axis, double cos_max) {
    const int n = 1000000;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        vec3 d = p.generate();
        double v = p.value(d);
        if (v > 0 && dot(unit_vector(d), axis) > cos_max)
            sum += 1 / v;
    }
    return sum / n;
}

// Integral of p estimated with defensive importance sampling: directions are
// drawn half from the uniform sphere, half from p itself, so sharply peaked
// pdfs (low roughness) are still integrated with little noise. Invalid
// (zero) samples from p contribute nothing.
static double integral_defensive(const pdf& p) {
    const int n = 400000;
    sphere_pdf uniform;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        vec3 d = random_double() < 0.5 ? uniform.generate() : p.generate();
        if (d.length_squared() == 0)
            continue;
        sum += p.value(d) / (0.5 * uniform.value(d) + 0.5 * p.value(d));
    }
    return sum / n;
}

// Fraction of generate() calls that produce a valid (non-zero) direction.
// The pdf integrates to this fraction when value() matches generate().
static double valid_fraction(const pdf& p) {
    const int n = 200000;
    int valid = 0;
    for (int i = 0; i < n; i++)
        if (p.generate().length_squared() > 0)
            valid++;
    return double(valid) / n;
}

static bool check(const std::string& name, double got, double want, double tolerance) {
    bool ok = std::fabs(got - want) < tolerance;
    std::cout << (ok ? "PASS " : "FAIL ") << name << ": " << got << " (expected " << want << ")\n";
    return ok;
}

int main() {
    bool ok = true;
    auto mat = std::make_shared<lambertian>(color(1, 1, 1));
    point3 origin(0, 0, 0);

    cosine_pdf cosine(vec3(0, 1, 0));
    ok &= check("cosine_pdf integral", integral(cosine), 1.0, 0.01);
    double hemisphere = cap_solid_angle(cosine, vec3(0, 1, 0), 0.0);
    ok &= check("cosine_pdf hemisphere", hemisphere, 2 * pi, 0.05);

    sphere ball(point3(0, 0, -3), 1.0, mat);
    hittable_pdf to_ball(ball, origin);
    ok &= check("sphere light pdf integral", integral(to_ball), 1.0, 0.02);

    sphere dome(point3(0.2, 0.1, 0), -3.0, mat);  // Seen from inside
    hittable_pdf to_dome(dome, origin);
    ok &= check("sphere light pdf integral from inside", integral(to_dome), 1.0, 0.02);
    ok &= check("sphere light sampling from inside", cap_solid_angle(to_dome, vec3(0, 1, 0), 0.0), 2 * pi, 0.05);

    quad panel(point3(-1, 2, -1), vec3(2, 0, 0), vec3(0, 0, 2), mat);
    hittable_pdf to_panel(panel, origin);
    ok &= check("quad light pdf integral", integral(to_panel), 1.0, 0.02);

    hittable_list both;
    both.add(std::make_shared<sphere>(point3(0, 0, -3), 1.0, mat));
    both.add(std::make_shared<quad>(point3(-1, 2, -1), vec3(2, 0, 0), vec3(0, 0, 2), mat));
    hittable_pdf to_both(both, origin);
    ok &= check("light list pdf integral", integral(to_both), 1.0, 0.02);

    auto p0 = std::make_shared<cosine_pdf>(vec3(0, 1, 0));
    auto p1 = std::make_shared<hittable_pdf>(ball, origin);
    mixture_pdf mix(p0, p1);
    ok &= check("mixture_pdf integral", integral(mix), 1.0, 0.02);
    ok &= check("mixture_pdf sampling", cap_solid_angle(mix, vec3(0, 1, 0), 0.0), 2 * pi, 0.05);

    // A dim sky with a small, very bright sun: the sampler should send most
    // directions at the sun, and value() must still integrate to one.
    const int sky_w = 64, sky_h = 32;
    std::vector<color> sky_pixels(sky_w * sky_h, color(0.2, 0.3, 0.5));
    for (int j = 5; j < 7; j++)
        for (int i = 40; i < 43; i++)
            sky_pixels[j * sky_w + i] = color(5000, 4000, 3000);
    environment_light sky(sky_w, sky_h, sky_pixels, 1.0, 30.0);
    hittable_pdf to_sky(sky, origin);
    ok &= check("environment pdf integral", integral_defensive(to_sky), 1.0, 0.02);
    ok &= check("environment sampling", cap_solid_angle(to_sky, vec3(0, 1, 0), -1.0), 4 * pi, 0.15);
    ok &= check("environment upper hemisphere", cap_solid_angle(to_sky, vec3(0, 1, 0), 0.0), 2 * pi, 0.15);

    vec3 normal(0, 0, 1);
    for (double roughness : {0.2, 0.6, 1.0}) {
        auto r = std::to_string(roughness).substr(0, 3);
        double alpha = roughness_to_alpha(roughness);
        for (vec3 wo : {vec3(0, 0, 1), unit_vector(vec3(1, 0, 1)), unit_vector(vec3(1, 0.2, 0.15))}) {
            ggx_reflection_pdf ggx_pdf(normal, wo, alpha);
            ok &= check("GGX reflection pdf, roughness " + r, integral_defensive(ggx_pdf), valid_fraction(ggx_pdf), 0.02);

            for (double eta : {1.5, 1 / 1.5}) {
                rough_dielectric_pdf glass(rough_dielectric_bsdf(normal, wo, eta, alpha));
                ok &= check("rough glass pdf, roughness " + r + ", eta " + std::to_string(eta).substr(0, 4),
                            integral_defensive(glass), valid_fraction(glass), 0.02);
            }
        }
    }

    return ok ? 0 : 1;
}
