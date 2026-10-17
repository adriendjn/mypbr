#ifndef PLANE_H
#define PLANE_H

#include "hittable.h"
#include "material.h"

#include <memory>

// The infinite horizontal plane y = height: a ground that runs to the
// horizon. It has no finite bounding box, so it can't go in the BVH; the
// scene keeps it apart (scene::unbounded) and main tests it beside the BVH.
// (u, v) = (x, -z), so textures repeat across it and normal maps work.
class ground_plane : public hittable{
    public:
        ground_plane(double height, std::shared_ptr<material> mat) : height(height), mat(mat) {}

        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            auto dy = r.direction().y();
            if (std::fabs(dy) < 1e-12)
                return false;
            auto t = (height - r.origin().y()) / dy;
            if (!ray_t.surrounds(t))
                return false;
            rec.t = t;
            rec.p = r.at(t);
            rec.p[1] = height;  // Exactly on the plane, so bounced rays start cleanly
            rec.u = rec.p.x();
            rec.v = -rec.p.z();
            rec.dpdu = vec3(1, 0, 0);
            rec.dpdv = vec3(0, 0, -1);  // cross(dpdu, dpdv) = +y
            rec.mat = mat.get();
            rec.set_face_normal(r, vec3(0, 1, 0));
            return true;
        }

        aabb bounding_box() const override { return aabb(interval::universe, interval(height, height), interval::universe); }

    private:
        double height;
        std::shared_ptr<material> mat;
};

#endif
