#ifndef AABB_H
#define AABB_H

#include "interval.h"
#include "ray.h"

// Axis-aligned bounding box.
class aabb{
    public:
        interval x, y, z;

        aabb() {}  // The default AABB is empty, since intervals are empty by default.

        aabb(const interval& x, const interval& y, const interval& z) : x(x), y(y), z(z) {
            pad_to_minimums();
        }

        // Treat the two points a and b as extrema for the bounding box.
        aabb(const point3& a, const point3& b) {
            x = (a[0] <= b[0]) ? interval(a[0], b[0]) : interval(b[0], a[0]);
            y = (a[1] <= b[1]) ? interval(a[1], b[1]) : interval(b[1], a[1]);
            z = (a[2] <= b[2]) ? interval(a[2], b[2]) : interval(b[2], a[2]);
            pad_to_minimums();
        }

        // The box enclosing both boxes.
        aabb(const aabb& box0, const aabb& box1) {
            x = interval(box0.x, box1.x);
            y = interval(box0.y, box1.y);
            z = interval(box0.z, box1.z);
        }

        const interval& axis_interval(int n) const {
            if (n == 1) return y;
            if (n == 2) return z;
            return x;
        }

        // Slab test. inv_dir is 1 / ray direction, precomputed by the caller.
        bool hit(const point3& origin, const vec3& inv_dir, interval ray_t) const {
            for (int axis = 0; axis < 3; axis++) {
                const interval& ax = axis_interval(axis);
                auto t0 = (ax.min - origin[axis]) * inv_dir[axis];
                auto t1 = (ax.max - origin[axis]) * inv_dir[axis];

                if (t0 > t1) std::swap(t0, t1);
                if (t0 > ray_t.min) ray_t.min = t0;
                if (t1 < ray_t.max) ray_t.max = t1;

                if (ray_t.max <= ray_t.min)
                    return false;
            }
            return true;
        }

        bool hit(const ray& r, interval ray_t) const {
            const vec3& d = r.direction();
            return hit(r.origin(), vec3(1/d.x(), 1/d.y(), 1/d.z()), ray_t);
        }

        point3 centroid() const {
            return point3((x.min + x.max)/2, (y.min + y.max)/2, (z.min + z.max)/2);
        }

        double surface_area() const {
            auto dx = x.size(), dy = y.size(), dz = z.size();
            return 2 * (dx*dy + dy*dz + dz*dx);
        }

        // Returns the index of the longest axis of the bounding box.
        int longest_axis() const {
            if (x.size() > y.size())
                return x.size() > z.size() ? 0 : 2;
            else
                return y.size() > z.size() ? 1 : 2;
        }

        static const aabb empty, universe;

    private:
        // Adjust the AABB so that no side is narrower than some delta, padding if necessary
        // (a quad lying in an axis plane would otherwise have zero thickness).
        void pad_to_minimums() {
            double delta = 0.0001;
            if (x.size() < delta) x = x.expand(delta);
            if (y.size() < delta) y = y.expand(delta);
            if (z.size() < delta) z = z.expand(delta);
        }
};

inline const aabb aabb::empty    = aabb(interval::empty,    interval::empty,    interval::empty);
inline const aabb aabb::universe = aabb(interval::universe, interval::universe, interval::universe);

#endif
