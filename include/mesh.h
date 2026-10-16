#ifndef MESH_H
#define MESH_H

#include "bvh.h"
#include "hittable.h"
#include "material.h"

#include <algorithm>
#include <memory>
#include <vector>

// Vertex data shared by the triangles of a mesh.
class mesh_data{
    public:
        std::vector<point3> positions;
        std::vector<vec3>   normals;  // Per-vertex shading normals (may be empty)
        std::vector<vec3>   uvs;      // Texture coordinates in x, y (may be empty)
};

// One triangle of a mesh, by vertex indices into mesh_data. Normal and uv
// indices are -1 when the file doesn't provide them.
class triangle : public hittable{
    public:
        triangle(const mesh_data* data, const int v[3], const int n[3], const int t[3], const material* mat)
            : data(data), mat(mat)
        {
            for (int k = 0; k < 3; k++) {
                vi[k] = v[k];
                ni[k] = n[k];
                ti[k] = t[k];
            }
            const point3& p0 = data->positions[vi[0]];
            const point3& p1 = data->positions[vi[1]];
            const point3& p2 = data->positions[vi[2]];
            bbox = aabb(aabb(p0, p1), aabb(p2, p2));
            area = 0.5 * cross(p1 - p0, p2 - p0).length();
        }

        // Möller-Trumbore ray/triangle intersection.
        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            const point3& p0 = data->positions[vi[0]];
            vec3 e1 = data->positions[vi[1]] - p0;
            vec3 e2 = data->positions[vi[2]] - p0;

            vec3 pvec = cross(r.direction(), e2);
            double det = dot(e1, pvec);
            if (std::fabs(det) < 1e-12)
                return false;  // Ray parallel to the triangle
            double inv_det = 1 / det;

            vec3 tvec = r.origin() - p0;
            double b1 = dot(tvec, pvec) * inv_det;
            if (b1 < 0 || b1 > 1)
                return false;

            vec3 qvec = cross(tvec, e1);
            double b2 = dot(r.direction(), qvec) * inv_det;
            if (b2 < 0 || b1 + b2 > 1)
                return false;

            double t = dot(e2, qvec) * inv_det;
            if (!ray_t.surrounds(t))
                return false;

            double b0 = 1 - b1 - b2;
            rec.t = t;
            rec.p = r.at(t);
            rec.mat = mat;

            // front_face comes from the geometric normal (counterclockwise winding)
            vec3 geometric = unit_vector(cross(e1, e2));
            rec.set_face_normal(r, geometric);

            // Interpolated shading normal, kept on the same side as the geometric one
            if (ni[0] >= 0 && ni[1] >= 0 && ni[2] >= 0) {
                vec3 shading = b0 * data->normals[ni[0]] + b1 * data->normals[ni[1]] + b2 * data->normals[ni[2]];
                if (shading.length_squared() > 0) {
                    shading = unit_vector(shading);
                    rec.normal = dot(shading, rec.normal) < 0 ? -shading : shading;
                }
            }

            if (ti[0] >= 0 && ti[1] >= 0 && ti[2] >= 0) {
                vec3 uv = b0 * data->uvs[ti[0]] + b1 * data->uvs[ti[1]] + b2 * data->uvs[ti[2]];
                rec.u = uv.x();
                rec.v = uv.y();
            } else {
                rec.u = b1;
                rec.v = b2;
            }
            return true;
        }

        aabb bounding_box() const override { return bbox; }

        double surface_area() const { return area; }

        // Area sampling converted to solid angle, as for quads.
        double pdf_value(const point3& origin, const vec3& direction) const override {
            hit_record rec;
            if (!this->hit(ray(origin, direction), interval(0.001, infinity), rec))
                return 0;
            vec3 geometric = unit_vector(cross(data->positions[vi[1]] - data->positions[vi[0]],
                                               data->positions[vi[2]] - data->positions[vi[0]]));
            auto distance_squared = rec.t * rec.t * direction.length_squared();
            auto cosine = std::fabs(dot(direction, geometric) / direction.length());
            return distance_squared / (cosine * area);
        }

        // Uniform point on the triangle.
        vec3 random(const point3& origin) const override {
            double r1 = random_double(), r2 = random_double();
            if (r1 + r2 > 1) {
                r1 = 1 - r1;
                r2 = 1 - r2;
            }
            const point3& p0 = data->positions[vi[0]];
            point3 p = p0 + r1 * (data->positions[vi[1]] - p0) + r2 * (data->positions[vi[2]] - p0);
            return p - origin;
        }

    private:
        const mesh_data* data;
        int vi[3], ni[3], ti[3];
        const material* mat;
        aabb bbox;
        double area;
};

// A triangle mesh with its own BVH. As a light, it samples triangles in
// proportion to their area.
class mesh : public hittable{
    public:
        mesh(std::shared_ptr<mesh_data> data, std::vector<std::shared_ptr<hittable>> triangles,
             std::shared_ptr<material> mat)
            : data(data), triangles(triangles), tree(triangles), mat(mat)
        {
            double total = 0;
            for (const auto& tri : triangles) {
                total += static_cast<const triangle&>(*tri).surface_area();
                area_cdf.push_back(total);
            }
            total_area = total;
        }

        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            return tree.hit(r, ray_t, rec);
        }

        aabb bounding_box() const override { return tree.bounding_box(); }

        size_t size() const { return triangles.size(); }

        // Sum over every triangle the direction passes through (a mesh light
        // need not be convex). Linear in the triangle count, which is fine
        // for the small meshes used as lights.
        double pdf_value(const point3& origin, const vec3& direction) const override {
            if (total_area <= 0)
                return 0;
            double sum = 0;
            for (const auto& tri : triangles) {
                const auto& t = static_cast<const triangle&>(*tri);
                sum += (t.surface_area() / total_area) * t.pdf_value(origin, direction);
            }
            return sum;
        }

        vec3 random(const point3& origin) const override {
            double x = random_double() * total_area;
            auto it = std::upper_bound(area_cdf.begin(), area_cdf.end(), x);
            size_t index = std::min(size_t(it - area_cdf.begin()), triangles.size() - 1);
            return triangles[index]->random(origin);
        }

    private:
        std::shared_ptr<mesh_data> data;
        std::vector<std::shared_ptr<hittable>> triangles;
        bvh tree;
        std::shared_ptr<material> mat;  // Keeps the triangles' material alive
        std::vector<double> area_cdf;
        double total_area;
};

#endif
