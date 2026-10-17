#ifndef HITTABLE_H
#define HITTABLE_H

#include "ray.h"
#include "aabb.h"
#include "interval.h"

#include <memory>

class material;

class hit_record{
    public:
        point3 p;
        vec3 normal;
        const material* mat;  // Owned by the object that was hit; a raw pointer avoids
                              // atomic reference counting on every hit across threads
        double t;
        double u, v;  // Surface (texture) coordinates
        // How p moves with u and v (dp/du, dp/dv), unnormalized: the tangent
        // and bitangent of tangent-space normal maps. Zero when the surface
        // has no usable (u, v) parameterization.
        vec3 dpdu, dpdv;
        bool front_face;

        // Sets the normal so it always points against the incoming ray.
        // outward_normal is assumed to have unit length.
        void set_face_normal(const ray& r, const vec3& outward_normal) {
            front_face = dot(r.direction(), outward_normal) < 0;
            normal = front_face ? outward_normal : -outward_normal;
        }
};

class hittable{
    public:
        virtual ~hittable() = default;

        virtual bool hit(const ray& r, interval ray_t, hit_record& rec) const = 0;

        virtual aabb bounding_box() const = 0;

        // Density (solid angle) of sampling direction from origin with random().
        // Only needed for objects used as lights.
        virtual double pdf_value(const point3& origin, const vec3& direction) const {
            (void)origin; (void)direction;
            return 0.0;
        }

        // Returns a direction from origin towards a random point on the object.
        virtual vec3 random(const point3& origin) const {
            (void)origin;
            return vec3(1, 0, 0);
        }
};

#endif
