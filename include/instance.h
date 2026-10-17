#ifndef INSTANCE_H
#define INSTANCE_H

#include "hittable.h"
#include "transform.h"

#include <memory>

// Places a shared object in the world through an affine transform, so the
// same geometry (e.g. a mesh) can appear several times without copies. Rays
// are moved into the object's space instead of moving the object.
class instance : public hittable{
    public:
        instance(std::shared_ptr<hittable> object, const transform& xform)
            : object(object), xform(xform)
        {
            // Bounding box of the eight transformed corners of the object's box
            aabb box = object->bounding_box();
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++)
                    for (int k = 0; k < 2; k++) {
                        point3 corner(i ? box.x.max : box.x.min, j ? box.y.max : box.y.min,
                                      k ? box.z.max : box.z.min);
                        point3 p = xform.apply_point(corner);
                        bbox = aabb(bbox, aabb(p, p));
                    }
        }

        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            // The object-space direction is not renormalized, so t is the same in both spaces
            ray local(xform.apply_inverse_point(r.origin()), xform.apply_inverse_vector(r.direction()));
            if (!object->hit(local, ray_t, rec))
                return false;

            rec.p = xform.apply_point(rec.p);
            rec.normal = unit_vector(xform.apply_normal(rec.normal));
            // Tangents are directions on the surface, moved like any vector
            rec.dpdu = xform.apply_vector(rec.dpdu);
            rec.dpdv = xform.apply_vector(rec.dpdv);
            return true;
        }

        aabb bounding_box() const override { return bbox; }

        // Exact for similarity transforms only; the scene loader refuses
        // emissive instances with non-uniform scale.
        double pdf_value(const point3& origin, const vec3& direction) const override {
            return object->pdf_value(xform.apply_inverse_point(origin), xform.apply_inverse_vector(direction));
        }

        vec3 random(const point3& origin) const override {
            return xform.apply_vector(object->random(xform.apply_inverse_point(origin)));
        }

        // Radiance is unchanged by the transform; power scales with area.
        double power() const override { return object->power() * xform.area_scale(); }

    private:
        std::shared_ptr<hittable> object;
        transform xform;
        aabb bbox;
};

#endif
