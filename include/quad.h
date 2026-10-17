#ifndef QUAD_H
#define QUAD_H

#include "hittable.h"
#include "hittable_list.h"
#include "material.h"

#include <memory>

// A parallelogram spanned by corner Q and edge vectors u and v.
class quad : public hittable{
    public:
        quad(const point3& Q, const vec3& u, const vec3& v, std::shared_ptr<material> mat)
            : Q(Q), u(u), v(v), mat(mat)
        {
            auto n = cross(u, v);
            normal = unit_vector(n);
            D = dot(normal, Q);
            w = n / dot(n, n);
            area = n.length();

            // Compute the bounding box of all four vertices.
            auto bbox_diagonal1 = aabb(Q, Q + u + v);
            auto bbox_diagonal2 = aabb(Q + u, Q + v);
            bbox = aabb(bbox_diagonal1, bbox_diagonal2);
        }

        aabb bounding_box() const override { return bbox; }

        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            auto denom = dot(normal, r.direction());

            // No hit if the ray is parallel to the plane.
            if (std::fabs(denom) < 1e-8)
                return false;

            // Return false if the hit point parameter t is outside the ray interval.
            auto t = (D - dot(normal, r.origin())) / denom;
            if (!ray_t.contains(t))
                return false;

            // Determine if the hit point lies within the planar shape using its plane coordinates.
            auto intersection = r.at(t);
            vec3 planar_hitpt_vector = intersection - Q;
            auto alpha = dot(w, cross(planar_hitpt_vector, v));
            auto beta = dot(w, cross(u, planar_hitpt_vector));

            static const interval unit_interval(0, 1);
            if (!unit_interval.contains(alpha) || !unit_interval.contains(beta))
                return false;

            rec.t = t;
            rec.p = intersection;
            rec.u = alpha;
            rec.v = beta;
            rec.dpdu = u;  // p = Q + alpha u + beta v
            rec.dpdv = v;
            rec.mat = mat.get();
            rec.set_face_normal(r, normal);

            return true;
        }

        // Area sampling converted to solid angle: distance^2 / (cos * area).
        double pdf_value(const point3& origin, const vec3& direction) const override {
            hit_record rec;
            if (!this->hit(ray(origin, direction), interval(0.001, infinity), rec))
                return 0;

            auto distance_squared = rec.t * rec.t * direction.length_squared();
            auto cosine = std::fabs(dot(direction, rec.normal) / direction.length());

            return distance_squared / (cosine * area);
        }

        vec3 random(const point3& origin) const override {
            auto p = Q + (random_double() * u) + (random_double() * v);
            return p - origin;
        }

        double power() const override { return pi * area * luminance(mat->average_emission()); }

    private:
        point3 Q;
        vec3 u, v;
        vec3 w;       // Cached cross(u, v) / |cross(u, v)|^2 for plane coordinates
        std::shared_ptr<material> mat;
        vec3 normal;
        double D;     // Plane equation: dot(normal, p) = D
        double area;
        aabb bbox;
};

// Returns the 3D box (six sides) that contains the two opposite vertices a & b.
inline std::shared_ptr<hittable_list> box(const point3& a, const point3& b, std::shared_ptr<material> mat) {
    auto sides = std::make_shared<hittable_list>();

    auto min = point3(std::fmin(a.x(),b.x()), std::fmin(a.y(),b.y()), std::fmin(a.z(),b.z()));
    auto max = point3(std::fmax(a.x(),b.x()), std::fmax(a.y(),b.y()), std::fmax(a.z(),b.z()));

    auto dx = vec3(max.x() - min.x(), 0, 0);
    auto dy = vec3(0, max.y() - min.y(), 0);
    auto dz = vec3(0, 0, max.z() - min.z());

    sides->add(std::make_shared<quad>(point3(min.x(), min.y(), max.z()),  dx,  dy, mat)); // front
    sides->add(std::make_shared<quad>(point3(max.x(), min.y(), max.z()), -dz,  dy, mat)); // right
    sides->add(std::make_shared<quad>(point3(max.x(), min.y(), min.z()), -dx,  dy, mat)); // back
    sides->add(std::make_shared<quad>(point3(min.x(), min.y(), min.z()),  dz,  dy, mat)); // left
    sides->add(std::make_shared<quad>(point3(min.x(), max.y(), max.z()),  dx, -dz, mat)); // top
    sides->add(std::make_shared<quad>(point3(min.x(), min.y(), min.z()),  dx,  dz, mat)); // bottom

    return sides;
}

#endif
