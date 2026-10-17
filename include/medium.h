#ifndef MEDIUM_H
#define MEDIUM_H

#include "hittable.h"
#include "material.h"

#include <memory>

// Scattering inside a participating medium (fog, smoke, haze): there is no
// surface, so a scattered direction is drawn from the phase function instead
// of a BSDF. albedo is the fraction of light scattered rather than absorbed at
// each event; g is the Henyey-Greenstein anisotropy (0 = isotropic).
class phase_material : public material{
    public:
        phase_material(const color& albedo, double g = 0) : albedo(albedo), g(g) {}

        bool is_volume() const override { return true; }

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            (void)rec;
            srec.attenuation = albedo;
            srec.pdf_ptr = std::make_shared<henyey_greenstein_pdf>(r_in.direction(), g);
            srec.skip_pdf = false;
            return true;
        }

        // albedo * phase: no cosine factor, since a volume has no surface to
        // project onto. The phase function is a pdf, so sampling it makes
        // eval / pdf = albedo.
        color eval(const ray& r_in, const hit_record& rec, const vec3& direction) const override {
            (void)rec;
            auto cos_theta = dot(unit_vector(r_in.direction()), unit_vector(direction));
            return albedo * henyey_greenstein_pdf::phase(cos_theta, g);
        }

    private:
        color albedo;
        double g;
};

// A homogeneous medium filling a closed, convex boundary.
//
// Light travelling a distance d through it survives with probability
// exp(-density * d) (Beer-Lambert). hit() samples the free-flight distance
// to the next collision from that exponential, d = -ln(1 - xi) / density:
// if the ray leaves the boundary (or meets a surface) first, the medium is
// not hit; otherwise it reports a scattering event at that point. The
// probability of passing through equals the transmittance, so this is an
// unbiased estimate of it, and shadow rays (NEE) need no special case: a
// shadow ray that scatters in the fog simply counts as blocked.
//
// To move or rotate a medium, transform the boundary, not the medium: hit()
// measures distances along the ray, and an instance changes their scale.
class constant_medium : public hittable{
    public:
        constant_medium(std::shared_ptr<hittable> boundary, double density,
                        std::shared_ptr<material> phase)
            : boundary(boundary), density(density), phase(phase) {}

        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            // Where the ray's line enters and leaves the boundary, even behind
            // the origin, so rays starting inside the medium work too
            hit_record rec1, rec2;
            if (!boundary->hit(r, interval::universe, rec1))
                return false;
            if (!boundary->hit(r, interval(rec1.t + 0.0001, infinity), rec2))
                return false;

            // The part of the segment inside the boundary and inside ray_t
            auto t0 = std::fmax(rec1.t, ray_t.min);
            auto t1 = std::fmin(rec2.t, ray_t.max);
            if (t0 >= t1)
                return false;

            auto ray_length = r.direction().length();
            auto distance_inside = (t1 - t0) * ray_length;
            auto hit_distance = -std::log(1 - random_double()) / density;
            if (hit_distance > distance_inside)
                return false;

            rec.t = t0 + hit_distance / ray_length;
            rec.p = r.at(rec.t);
            rec.normal = vec3(1, 0, 0);  // Arbitrary: nothing uses a medium's normal
            rec.front_face = true;
            rec.u = rec.v = 0;
            rec.mat = phase.get();
            return true;
        }

        aabb bounding_box() const override { return boundary->bounding_box(); }

    private:
        std::shared_ptr<hittable> boundary;
        double density;  // Extinction coefficient: collisions per unit length
        std::shared_ptr<material> phase;
};

// Fog whose density falls off with height, density(y) = density0 *
// exp(-(y - base) / scale_height), filling all of space: thick in the
// valleys, thin on the hills, and towards the horizon, where a ray crosses
// a long stretch of low air, it fades the ground into the sky.
//
// Free-flight sampling works as in constant_medium, but with a varying
// density the survival probability is exp(-tau), tau the optical depth (the
// integral of the density along the ray). Along a ray o + s d it is an
// exponential in s, so tau has a closed form and can be inverted: draw
// tau* = -ln(1 - xi) and solve tau(t) = tau* for the collision distance t.
// Going up, the total optical depth is finite, so a ray may leave without
// a collision, with probability exp(-total): the transmittance.
//
// It has no bounding box (scene::unbounded holds it, beside the BVH).
class height_fog : public hittable{
    public:
        height_fog(double density, double scale_height, double base, std::shared_ptr<material> phase)
            : density(density), scale_height(scale_height), base(base), phase(phase) {}

        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            double length = r.direction().length();
            double k = r.direction().y() / scale_height;  // Density changes as exp(-k s) along the ray
            // Density at the start of the segment, per unit of s (ray parameter)
            double t0 = ray_t.min;
            double start = density * length * std::exp(-(r.origin().y() + t0 * r.direction().y() - base) / scale_height);
            if (!(start > 0))
                return false;
            double tau = -std::log(1 - random_double());
            double t;
            if (std::fabs(k) < 1e-12) {
                t = t0 + tau / start;
            } else {
                // tau(t) = start * (1 - exp(-k (t - t0))) / k
                double x = 1 - tau * k / start;
                if (x <= 0)
                    return false;  // Leaves upwards before the collision
                t = t0 - std::log(x) / k;
            }
            if (!(t < ray_t.max) || !std::isfinite(t))
                return false;

            rec.t = t;
            rec.p = r.at(t);
            rec.normal = vec3(1, 0, 0);  // Arbitrary: nothing uses a medium's normal
            rec.front_face = true;
            rec.u = rec.v = 0;
            rec.mat = phase.get();
            return true;
        }

        aabb bounding_box() const override { return aabb(interval::universe, interval::universe, interval::universe); }

    private:
        double density;       // At height base
        double scale_height;  // Height over which the density falls by e
        double base;
        std::shared_ptr<material> phase;
};

#endif
