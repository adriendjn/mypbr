#ifndef SPHERE_H
#define SPHERE_H

#include "hittable.h"
#include "material.h"
#include "onb.h"

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
            rec.mat = mat.get();

            return true;
        }

        double pdf_value(const point3& origin, const vec3& direction) const override {
            // This method only works for stationary spheres seen from outside.
            hit_record rec;
            if (!this->hit(ray(origin, direction), interval(0.001, infinity), rec))
                return 0;

            auto dist_squared = (center - origin).length_squared();
            auto cos_theta_max = std::sqrt(std::fmax(0.0, 1 - radius*radius/dist_squared));
            auto solid_angle = 2*pi*(1-cos_theta_max);

            return 1 / solid_angle;
        }

        // Samples the cone of directions from origin that the sphere subtends.
        vec3 random(const point3& origin) const override {
            vec3 direction = center - origin;
            auto distance_squared = direction.length_squared();
            onb uvw(direction);
            return uvw.transform(random_to_sphere(std::fabs(radius), distance_squared));
        }

    private:
        point3 center;
        double radius;
        std::shared_ptr<material> mat;
        aabb bbox;

        static vec3 random_to_sphere(double radius, double distance_squared) {
            auto r1 = random_double();
            auto r2 = random_double();
            auto z = 1 + r2*(std::sqrt(std::fmax(0.0, 1 - radius*radius/distance_squared)) - 1);

            auto phi = 2*pi*r1;
            auto x = std::cos(phi) * std::sqrt(1-z*z);
            auto y = std::sin(phi) * std::sqrt(1-z*z);

            return vec3(x, y, z);
        }
};

#endif
