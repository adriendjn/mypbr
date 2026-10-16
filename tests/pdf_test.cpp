// Checks that each pdf integrates to one over the sphere of directions and
// that its value() matches the density of the directions generate() draws.

#include <hittable_list.h>
#include <material.h>
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

    return ok ? 0 : 1;
}
