#ifndef STUDIO_H
#define STUDIO_H

// Photo studio pieces: a cyclorama (a backdrop that curves into the floor)
// and softboxes (rectangular area lights aimed at a point).

#include "material.h"
#include "mesh.h"
#include "quad.h"

#include <cmath>
#include <memory>
#include <vector>

// The shape of a cyclorama, in its own frame: the floor is y = 0 from
// z = front back to z = -back, where a quarter circle of the given radius
// bends it up into a wall at z = -back - radius, which rises to y = height.
// It is width wide, centered on x = 0.
class cyclorama_settings{
    public:
        double width  = 20;
        double front  = 10;
        double back   = 4;
        double height = 8;
        double radius = 3;
        int    segments = 32;  // Flat strips along the curve
};

// A cyclorama as one mesh. Without a visible edge between floor and wall
// there is no horizon: the floor seems to go on forever and the car floats in
// a seamless space, which is why car and product photographers use them.
//
// The curve is a strip of flat triangles, but each profile vertex carries
// the circle's true normal (pointing at the circle's center) and triangles
// interpolate it, so the shading is smooth; the flat pieces only show in the
// silhouette, which the camera never sees from the front. The floor's last
// row and the curve's first share the normal (0, 1, 0), and the curve's last
// and the wall's share (0, 0, 1): no crease at either seam. uv runs along x
// (u) and along the profile (v, by arc length), both from 0 to 1.
inline std::shared_ptr<mesh> make_cyclorama(const cyclorama_settings& s, std::shared_ptr<material> mat) {
    // The profile in the (y, z) plane, from the front of the floor to the top of the wall
    std::vector<double> ys, zs, nys, nzs;
    auto add = [&](double y, double z, double ny, double nz) {
        ys.push_back(y);
        zs.push_back(z);
        nys.push_back(ny);
        nzs.push_back(nz);
    };
    add(0, s.front, 1, 0);
    for (int i = 0; i <= s.segments; i++) {
        // Angle up the curve: 0 on the floor, pi/2 on the wall. The circle's
        // center is (y = radius, z = -back), and the normal points at it.
        double theta = (pi / 2) * i / s.segments;
        add(s.radius * (1 - std::cos(theta)), -s.back - s.radius * std::sin(theta),
            std::cos(theta), std::sin(theta));
    }
    if (s.height > s.radius)
        add(s.height, -s.back - s.radius, 0, 1);

    std::vector<double> arc(ys.size(), 0);
    for (size_t i = 1; i < ys.size(); i++)
        arc[i] = arc[i - 1] + std::hypot(ys[i] - ys[i - 1], zs[i] - zs[i - 1]);

    auto data = std::make_shared<mesh_data>();
    for (size_t i = 0; i < ys.size(); i++) {
        for (int side = 0; side < 2; side++) {
            double x = side == 0 ? -s.width / 2 : s.width / 2;
            data->positions.push_back(point3(x, ys[i], zs[i]));
            data->normals.push_back(vec3(0, nys[i], nzs[i]));
            data->uvs.push_back(vec3(side, arc[i] / arc.back(), 0));
        }
    }

    // Two triangles per strip between profile rows i and i + 1, wound
    // counterclockwise seen from the front (geometric normal up / forward)
    std::vector<std::shared_ptr<hittable>> triangles;
    for (int i = 0; i + 1 < int(ys.size()); i++) {
        int l0 = 2 * i, r0 = 2 * i + 1, l1 = 2 * i + 2, r1 = 2 * i + 3;
        int a[3] = {l0, r0, r1}, b[3] = {l0, r1, l1};
        triangles.push_back(std::make_shared<triangle>(data.get(), a, a, a, mat.get()));
        triangles.push_back(std::make_shared<triangle>(data.get(), b, b, b, mat.get()));
    }
    return std::make_shared<mesh>(data, std::move(triangles), mat);
}

// A width x height rectangle centered on center, facing aim (its emitting
// front face looks at that point). The width runs horizontally, so a tall
// narrow box is a strip light standing up; roll turns the box about its
// aiming axis, in degrees. For a box aimed straight down, width runs along x.
inline std::shared_ptr<quad> make_softbox(const point3& center, const point3& aim, double width, double height,
                                          double roll_degrees, std::shared_ptr<material> mat) {
    vec3 dir = unit_vector(aim - center);
    vec3 up = std::fabs(dir.y()) > 0.999 ? vec3(0, 0, 1) : vec3(0, 1, 0);
    vec3 u = unit_vector(cross(up, dir));
    vec3 v = cross(dir, u);  // cross(u, v) = dir: the quad's normal faces the aim point
    double roll = degrees_to_radians(roll_degrees);
    vec3 ur = std::cos(roll) * u + std::sin(roll) * v;
    vec3 vr = -std::sin(roll) * u + std::cos(roll) * v;
    point3 corner = center - (width / 2) * ur - (height / 2) * vr;
    return std::make_shared<quad>(corner, width * ur, height * vr, mat);
}

#endif
