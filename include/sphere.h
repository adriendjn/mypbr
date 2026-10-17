#ifndef SPHERE_H
#define SPHERE_H

#include "hittable.h"
#include "material.h"
#include "onb.h"

#include <algorithm>

class sphere : public hittable{
    public:
        // A negative radius keeps the geometry but flips the normal inward,
        // which makes a hollow shell when nested inside a dielectric sphere.
        sphere(const point3& center, double radius, std::shared_ptr<material> mat)
            : center(center), radius(radius), mat(mat)
        {
            auto rvec = vec3(std::fabs(radius), std::fabs(radius), std::fabs(radius));
            bbox = aabb(center - rvec, center + rvec);
        }

        aabb bounding_box() const override { return bbox; }

        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            vec3 oc = center - r.origin();
            auto a = r.direction().length_squared();
            auto h = dot(r.direction(), oc);
            auto c = oc.length_squared() - radius*radius;

            auto discriminant = h*h - a*c;
            if (discriminant < 0)
                return false;

            auto sqrtd = std::sqrt(discriminant);

            // Find the nearest root that lies in the acceptable range
            auto root = (h - sqrtd) / a;
            if (!ray_t.surrounds(root)) {
                root = (h + sqrtd) / a;
                if (!ray_t.surrounds(root))
                    return false;
            }

            rec.t = root;
            rec.p = r.at(rec.t);
            vec3 outward_normal = (rec.p - center) / radius;
            rec.set_face_normal(r, outward_normal);
            get_sphere_uv(outward_normal * (radius < 0 ? -1.0 : 1.0), rec.u, rec.v);
            rec.mat = mat.get();

            return true;
        }

        // Samples the cone of directions the sphere subtends from outside it;
        // from inside, samples points uniformly over its whole surface.
        double pdf_value(const point3& origin, const vec3& direction) const override {
            hit_record rec;
            if (!this->hit(ray(origin, direction), interval(0.001, infinity), rec))
                return 0;

            double r = std::fabs(radius);
            auto dist_squared = (center - origin).length_squared();
            if (dist_squared <= r*r) {
                // Area density converted to solid angle: distance^2 / (cos * area)
                auto hit_dist_squared = rec.t * rec.t * direction.length_squared();
                auto cosine = std::fabs(dot(unit_vector(direction), (rec.p - center) / r));
                return hit_dist_squared / (cosine * 4*pi*r*r);
            }

            auto cos_theta_max = std::sqrt(1 - r*r/dist_squared);
            auto solid_angle = 2*pi*(1-cos_theta_max);
            return 1 / solid_angle;
        }

        vec3 random(const point3& origin) const override {
            double r = std::fabs(radius);
            vec3 direction = center - origin;
            auto distance_squared = direction.length_squared();
            if (distance_squared <= r*r)
                return center + r * random_unit_vector() - origin;
            onb uvw(direction);
            return uvw.transform(random_to_sphere(r, distance_squared));
        }

        // pi * area * L. A hollow (negative radius) sphere emits inwards, over the same area.
        double power() const override {
            return pi * 4*pi*radius*radius * luminance(mat->average_emission());
        }

    private:
        point3 center;
        double radius;
        std::shared_ptr<material> mat;
        aabb bbox;

        // p: a point on the unit sphere centered at the origin.
        // u: returned value [0,1] of angle around the Y axis from X=-1.
        // v: returned value [0,1] of angle from Y=-1 to Y=+1.
        static void get_sphere_uv(const point3& p, double& u, double& v) {
            auto theta = std::acos(std::clamp(-p.y(), -1.0, 1.0));
            auto phi = std::atan2(-p.z(), p.x()) + pi;

            u = phi / (2*pi);
            v = theta / pi;
        }

        static vec3 random_to_sphere(double radius, double distance_squared) {
            auto r1 = random_double();
            auto r2 = random_double();
            auto z = 1 + r2*(std::sqrt(1 - radius*radius/distance_squared) - 1);

            auto phi = 2*pi*r1;
            auto x = std::cos(phi) * std::sqrt(1-z*z);
            auto y = std::sin(phi) * std::sqrt(1-z*z);

            return vec3(x, y, z);
        }
};

#endif
