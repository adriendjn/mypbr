#ifndef MICROFACET_H
#define MICROFACET_H

#include "color3.h"
#include "onb.h"
#include "pdf.h"

#include <algorithm>

// Microfacet building blocks. Directions are in a local shading frame where
// the surface normal is +z, both wo (towards the viewer) and wi (towards the
// light) point away from the surface, and all vectors are unit length.

// Converts a perceptual roughness in [0,1] to the GGX alpha parameter.
inline double roughness_to_alpha(double roughness) {
    return std::max(roughness * roughness, 1e-3);
}

// Schlick's Fresnel approximation for a normal-incidence reflectance f0.
inline double fresnel_schlick(double cos_theta, double f0) {
    return f0 + (1 - f0) * std::pow(1 - std::clamp(cos_theta, 0.0, 1.0), 5);
}

inline color fresnel_schlick(double cos_theta, const color& f0) {
    return color(fresnel_schlick(cos_theta, f0.x()), fresnel_schlick(cos_theta, f0.y()),
                 fresnel_schlick(cos_theta, f0.z()));
}

// Exact Fresnel reflectance of an unpolarized ray hitting a dielectric
// boundary. eta is the index on the transmitted side over the index on the
// incident side, for cos_i > 0. Returns 1 for total internal reflection.
inline double fresnel_dielectric(double cos_i, double eta) {
    cos_i = std::clamp(cos_i, -1.0, 1.0);
    if (cos_i < 0) {
        eta = 1 / eta;
        cos_i = -cos_i;
    }
    double sin2_i = 1 - cos_i * cos_i;
    double sin2_t = sin2_i / (eta * eta);
    if (sin2_t >= 1)
        return 1;
    double cos_t = std::sqrt(1 - sin2_t);
    double r_parl = (eta * cos_i - cos_t) / (eta * cos_i + cos_t);
    double r_perp = (cos_i - eta * cos_t) / (cos_i + eta * cos_t);
    return (r_parl * r_parl + r_perp * r_perp) / 2;
}

// Isotropic GGX (Trowbridge-Reitz) distribution with Smith masking-shadowing.
class ggx{
    public:
        explicit ggx(double alpha) : alpha(alpha) {}

        // Density of microfacet normals h (projected-area normalized).
        double D(const vec3& h) const {
            if (h.z() <= 0)
                return 0;
            double a2 = alpha * alpha;
            double d = h.z() * h.z() * (a2 - 1) + 1;
            return a2 / (pi * d * d);
        }

        double lambda(const vec3& w) const {
            double cos2 = w.z() * w.z();
            if (cos2 <= 0)
                return infinity;
            double tan2 = std::fmax(0.0, 1 - cos2) / cos2;
            return (-1 + std::sqrt(1 + alpha * alpha * tan2)) / 2;
        }

        double G1(const vec3& w) const { return 1 / (1 + lambda(w)); }

        // Height-correlated masking-shadowing.
        double G(const vec3& wo, const vec3& wi) const { return 1 / (1 + lambda(wo) + lambda(wi)); }

        // Density of the normals visible from wo.
        double visible_pdf(const vec3& wo, const vec3& h) const {
            if (wo.z() <= 0)
                return 0;
            return G1(wo) / wo.z() * D(h) * std::fabs(dot(wo, h));
        }

        // Samples a normal from the distribution of normals visible from wo
        // (Heitz 2018, "Sampling the GGX Distribution of Visible Normals").
        vec3 sample_visible(const vec3& wo) const {
            double u1 = random_double();
            double u2 = random_double();
            return sample_visible(wo, u1, u2);
        }

        // The same warp driven by given uniform numbers u1, u2 in [0,1), for
        // deterministic (stratified) sampling.
        vec3 sample_visible(const vec3& wo, double u1, double u2) const {
            // Stretch the view direction to the hemisphere configuration
            vec3 vh = unit_vector(vec3(alpha * wo.x(), alpha * wo.y(), wo.z()));

            double lensq = vh.x() * vh.x() + vh.y() * vh.y();
            vec3 t1 = lensq > 0 ? vec3(-vh.y(), vh.x(), 0) / std::sqrt(lensq) : vec3(1, 0, 0);
            vec3 t2 = cross(vh, t1);

            // Uniform point on the disk, warped towards the visible half
            double r = std::sqrt(u1);
            double phi = 2 * pi * u2;
            double p1 = r * std::cos(phi);
            double p2 = r * std::sin(phi);
            double s = 0.5 * (1 + vh.z());
            p2 = (1 - s) * std::sqrt(std::fmax(0.0, 1 - p1 * p1)) + s * p2;

            vec3 nh = p1 * t1 + p2 * t2 + std::sqrt(std::fmax(0.0, 1 - p1 * p1 - p2 * p2)) * vh;

            // Unstretch back to the ellipsoid configuration
            return unit_vector(vec3(alpha * nh.x(), alpha * nh.y(), std::fmax(1e-6, nh.z())));
        }

    private:
        double alpha;
};

// Samples mirror reflections about visible GGX normals.
class ggx_reflection_pdf : public pdf{
    public:
        ggx_reflection_pdf(const vec3& normal, const vec3& wo_world, double alpha)
            : frame(normal), wo(unit_vector(frame.to_local(wo_world))), dist(alpha) {}

        double value(const vec3& direction) const override {
            if (direction.length_squared() == 0)
                return 0;
            vec3 wi = unit_vector(frame.to_local(direction));
            if (wi.z() <= 0 || wo.z() <= 0)
                return 0;
            vec3 h = unit_vector(wo + wi);
            return dist.visible_pdf(wo, h) / (4 * std::fabs(dot(wo, h)));
        }

        // Returns the zero vector when the reflection falls below the surface.
        vec3 generate() const override {
            if (wo.z() <= 0)
                return vec3(0, 0, 0);
            vec3 h = dist.sample_visible(wo);
            vec3 wi = -wo + 2 * dot(wo, h) * h;
            if (wi.z() <= 0)
                return vec3(0, 0, 0);
            return frame.transform(wi);
        }

    private:
        onb frame;
        vec3 wo;
        ggx dist;
};

// Multiple-scattering energy compensation (Kulla and Conty 2017, "Revisiting
// Physically Based Shading at Imageworks").
//
// Single-scattering GGX drops the light that bounces more than once between
// microfacets, so rough surfaces reflect less than they should. With F = 1,
// the lobe's directional albedo E(mu) = integral of D G / (4 cos_o) over the
// hemisphere is what it keeps for a view at cos_o = mu; Kulla-Conty add a
// near-diffuse lobe that returns the missing 1 - E(mu):
//
//   f_ms(o, i) = (1 - E(mu_o)) (1 - E(mu_i)) / (pi (1 - E_avg)),
//   E_avg = 2 * integral of E(mu) mu dmu over [0,1].
//
// Integrating f_ms cos_i over the hemisphere gives exactly 1 - E(mu_o), and
// f_ms is symmetric in o and i, so it stays reciprocal.

// Tabulated E(mu, alpha) and E_avg(alpha) of the F = 1 single-scatter GGX
// lobe, built once on first use. The rows are spaced evenly in roughness
// (sqrt(alpha)), where E changes most evenly, and the columns in mu.
class ggx_albedo_table{
    public:
        static constexpr int size = 32;

        // Built on first call; C++11 makes this initialization thread-safe.
        static const ggx_albedo_table& get() {
            static const ggx_albedo_table table;
            return table;
        }

        // Bilinear in (roughness, mu).
        double E(double cos_theta, double alpha) const {
            double x = std::clamp(cos_theta, 0.0, 1.0) * (size - 1);
            double y = std::clamp(std::sqrt(alpha), 0.0, 1.0) * (size - 1);
            int i = std::min(int(x), size - 2);
            int j = std::min(int(y), size - 2);
            double tx = x - i, ty = y - j;
            double lo = (1 - tx) * e[j][i] + tx * e[j][i + 1];
            double hi = (1 - tx) * e[j + 1][i] + tx * e[j + 1][i + 1];
            return (1 - ty) * lo + ty * hi;
        }

        // Linear in roughness.
        double E_avg(double alpha) const {
            double y = std::clamp(std::sqrt(alpha), 0.0, 1.0) * (size - 1);
            int j = std::min(int(y), size - 2);
            double ty = y - j;
            return (1 - ty) * e_avg[j] + ty * e_avg[j + 1];
        }

    private:
        double e[size][size];  // [roughness][mu]
        double e_avg[size];

        ggx_albedo_table() {
            // Stratified grid of VNDF samples: a fixed, deterministic sequence
            // (the thread_local random_double() would make every run differ)
            const int strata = 64;
            for (int j = 0; j < size; j++) {
                double roughness = double(j) / (size - 1);
                ggx dist(roughness_to_alpha(roughness));
                for (int i = 0; i < size; i++) {
                    // mu = 0 is a grazing view no sample can see; use a nearby angle
                    double mu = std::fmax(double(i) / (size - 1), 1e-3);
                    vec3 wo(std::sqrt(1 - mu * mu), 0, mu);

                    // Sampling visible normals, f cos_i / pdf = (D G / (4 cos_o))
                    // / (G1(wo) D / (4 cos_o)) = G / G1(wo), with F = 1
                    double sum = 0;
                    for (int a = 0; a < strata; a++) {
                        for (int b = 0; b < strata; b++) {
                            vec3 h = dist.sample_visible(wo, (a + 0.5) / strata, (b + 0.5) / strata);
                            vec3 wi = -wo + 2 * dot(wo, h) * h;
                            if (wi.z() > 0)
                                sum += dist.G(wo, wi) / dist.G1(wo);
                        }
                    }
                    e[j][i] = std::fmin(sum / (strata * strata), 1.0);
                }

                // E_avg = 2 * integral of E(mu) mu dmu, taken exactly over the
                // piecewise linear E that E() interpolates, so the compensation
                // lobe returns 1 - E(mu_o) as E() itself reports it. On [a, b]
                // with E linear: integral of E mu = (b - a) (E_a (2a + b) + E_b (a + 2b)) / 6.
                double integral = 0;
                double h = 1.0 / (size - 1);
                for (int i = 0; i + 1 < size; i++) {
                    double a = i * h, b = (i + 1) * h;
                    integral += h * (e[j][i] * (2 * a + b) + e[j][i + 1] * (a + 2 * b)) / 6;
                }
                e_avg[j] = std::fmin(2 * integral, 1.0);
            }
        }
};

// f_ms(o, i) cos_i, the compensation lobe for F = 1 (see above).
inline double ggx_ms_eval(double cos_o, double cos_i, double alpha) {
    if (cos_o <= 0 || cos_i <= 0)
        return 0;
    const auto& table = ggx_albedo_table::get();
    double missing_avg = 1 - table.E_avg(alpha);
    if (missing_avg < 1e-6)
        return 0;  // Smooth enough that single scattering keeps all the energy
    return (1 - table.E(cos_o, alpha)) * (1 - table.E(cos_i, alpha)) / (pi * missing_avg) * cos_i;
}

// Energy the compensation lobe reflects for a view at cos_o with F = 1: 1 - E(cos_o).
inline double ggx_ms_albedo(double cos_o, double alpha) {
    if (cos_o <= 0 || 1 - ggx_albedo_table::get().E_avg(alpha) < 1e-6)
        return 0;
    return 1 - ggx_albedo_table::get().E(cos_o, alpha);
}

// Color of the multiply scattered light for a Schlick conductor. Each bounce
// between microfacets is tinted by the hemispherical average Fresnel F_avg,
// which for Schlick is f0 + (1 - f0) * 2 * integral of (1 - mu)^5 mu dmu
// = f0 + (1 - f0) / 21. Summing the bounces as a geometric series gives
// F_ms = F_avg^2 E_avg / (1 - F_avg (1 - E_avg)), which is 1 for f0 = 1.
inline color ggx_ms_fresnel(const color& f0, double alpha) {
    double e_avg = ggx_albedo_table::get().E_avg(alpha);
    color f_avg = f0 + (color(1, 1, 1) - f0) / 21;
    color f_ms;
    for (int k = 0; k < 3; k++)
        f_ms[k] = f_avg[k] * f_avg[k] * e_avg / (1 - f_avg[k] * (1 - e_avg));
    return f_ms;
}

// The tinted compensation lobe, F_ms f_ms(o, i) cos_i, to add to a GGX
// conductor's D G F / (4 cos_o).
inline color ggx_ms_term(double cos_o, double cos_i, double alpha, const color& f0) {
    return ggx_ms_fresnel(f0, alpha) * ggx_ms_eval(cos_o, cos_i, alpha);
}

// Rough dielectric (Walter et al. 2007, in the form used by pbrt-v4).
// The frame's +z is the normal on wo's side, and eta is the index of
// refraction on the far side over the index on wo's side.
class rough_dielectric_bsdf{
    public:
        rough_dielectric_bsdf(const vec3& normal, const vec3& wo_world, double eta, double alpha)
            : frame(normal), wo(unit_vector(frame.to_local(wo_world))), eta(eta), dist(alpha) {}

        // The BSDF times |cos(theta_i)| for direction wi (world space).
        double eval(const vec3& wi_world) const {
            vec3 wi, wm;
            bool reflect;
            if (!half_vector(wi_world, wi, wm, reflect))
                return 0;

            double F = fresnel_dielectric(dot(wo, wm), eta);
            if (reflect)
                return F * dist.D(wm) * dist.G(wo, wi) / (4 * wo.z());

            double denom = sqr(dot(wi, wm) + dot(wo, wm) / eta);
            double ft = (1 - F) * dist.D(wm) * dist.G(wo, wi)
                      * std::fabs(dot(wi, wm) * dot(wo, wm) / (wi.z() * wo.z() * denom));
            // Radiance is compressed into the smaller solid angle on the denser side
            ft /= eta * eta;
            return ft * std::fabs(wi.z());
        }

        double pdf_value(const vec3& wi_world) const {
            vec3 wi, wm;
            bool reflect;
            if (!half_vector(wi_world, wi, wm, reflect))
                return 0;

            double F = fresnel_dielectric(dot(wo, wm), eta);
            if (reflect)
                return F * dist.visible_pdf(wo, wm) / (4 * std::fabs(dot(wo, wm)));

            double denom = sqr(dot(wi, wm) + dot(wo, wm) / eta);
            return (1 - F) * dist.visible_pdf(wo, wm) * std::fabs(dot(wi, wm)) / denom;
        }

        // Returns the zero vector if the sample is invalid.
        vec3 sample() const {
            if (wo.z() <= 0)
                return vec3(0, 0, 0);
            vec3 wm = dist.sample_visible(wo);
            double F = fresnel_dielectric(dot(wo, wm), eta);

            vec3 wi;
            if (random_double() < F) {
                wi = -wo + 2 * dot(wo, wm) * wm;
                if (wi.z() <= 0)
                    return vec3(0, 0, 0);
            } else {
                wi = refract(-wo, wm, 1 / eta);
                if (wi.z() >= 0)
                    return vec3(0, 0, 0);
            }
            return frame.transform(wi);
        }

    private:
        onb frame;
        vec3 wo;
        double eta;
        ggx dist;

        static double sqr(double x) { return x * x; }

        // Finds the microfacet normal that maps wo to wi_world. Returns false
        // for configurations no microfacet can produce.
        bool half_vector(const vec3& wi_world, vec3& wi, vec3& wm, bool& reflect) const {
            if (wi_world.length_squared() == 0 || wo.z() <= 0)
                return false;
            wi = unit_vector(frame.to_local(wi_world));
            if (wi.z() == 0)
                return false;
            reflect = wi.z() > 0;
            wm = reflect ? wo + wi : wi * eta + wo;
            if (wm.length_squared() == 0)
                return false;
            wm = unit_vector(wm);
            if (wm.z() < 0)
                wm = -wm;
            // Discard back-facing microfacets
            if (dot(wm, wi) * wi.z() < 0 || dot(wm, wo) * wo.z() < 0)
                return false;
            return true;
        }
};

// Adapts rough_dielectric_bsdf's sampling to the pdf interface.
class rough_dielectric_pdf : public pdf{
    public:
        explicit rough_dielectric_pdf(const rough_dielectric_bsdf& bsdf) : bsdf(bsdf) {}

        double value(const vec3& direction) const override { return bsdf.pdf_value(direction); }
        vec3 generate() const override { return bsdf.sample(); }

    private:
        rough_dielectric_bsdf bsdf;
};

#endif
