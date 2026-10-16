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
            // Stretch the view direction to the hemisphere configuration
            vec3 vh = unit_vector(vec3(alpha * wo.x(), alpha * wo.y(), wo.z()));

            double lensq = vh.x() * vh.x() + vh.y() * vh.y();
            vec3 t1 = lensq > 0 ? vec3(-vh.y(), vh.x(), 0) / std::sqrt(lensq) : vec3(1, 0, 0);
            vec3 t2 = cross(vh, t1);

            // Uniform point on the disk, warped towards the visible half
            double r = std::sqrt(random_double());
            double phi = 2 * pi * random_double();
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
