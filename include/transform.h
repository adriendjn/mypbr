#ifndef TRANSFORM_H
#define TRANSFORM_H

#include "vec3.h"

#include <cmath>

// An affine transform p -> M p + t, stored with its inverse.
class transform{
    public:
        // The identity transform.
        transform() : m{{1,0,0},{0,1,0},{0,0,1}}, t(0,0,0), inv_m{{1,0,0},{0,1,0},{0,0,1}}, inv_t(0,0,0) {}

        static transform translate(const vec3& offset) {
            transform x;
            x.t = offset;
            x.inv_t = -offset;
            return x;
        }

        static transform scale(const vec3& s) {
            transform x;
            for (int k = 0; k < 3; k++) {
                x.m[k][k] = s[k];
                x.inv_m[k][k] = 1 / s[k];
            }
            return x;
        }

        // Rotation by angle (degrees) about axis, counterclockwise looking down the axis.
        static transform rotate(double degrees, const vec3& axis) {
            vec3 a = unit_vector(axis);
            double theta = degrees_to_radians(degrees);
            double c = std::cos(theta), s = std::sin(theta), C = 1 - c;
            transform x;
            double r[3][3] = {
                { a.x()*a.x()*C + c,       a.x()*a.y()*C - a.z()*s, a.x()*a.z()*C + a.y()*s },
                { a.y()*a.x()*C + a.z()*s, a.y()*a.y()*C + c,       a.y()*a.z()*C - a.x()*s },
                { a.z()*a.x()*C - a.y()*s, a.z()*a.y()*C + a.x()*s, a.z()*a.z()*C + c       },
            };
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    x.m[i][j] = r[i][j];
                    x.inv_m[j][i] = r[i][j];  // A rotation's inverse is its transpose
                }
            return x;
        }

        // The transform that applies first, then this one.
        transform then_after(const transform& first) const {
            transform x;
            x.m_mul(m, first.m, x.m);
            x.t = apply_vector(first.t) + t;
            x.m_mul(first.inv_m, inv_m, x.inv_m);
            x.inv_t = first.apply_inverse_vector(inv_t) + first.inv_t;
            return x;
        }

        point3 apply_point(const point3& p) const { return apply_vector(p) + t; }
        vec3 apply_vector(const vec3& v) const { return mul(m, v); }

        point3 apply_inverse_point(const point3& p) const { return apply_inverse_vector(p) + inv_t; }
        vec3 apply_inverse_vector(const vec3& v) const { return mul(inv_m, v); }

        // Normals transform by the inverse transpose.
        vec3 apply_normal(const vec3& n) const {
            return vec3(inv_m[0][0]*n[0] + inv_m[1][0]*n[1] + inv_m[2][0]*n[2],
                        inv_m[0][1]*n[0] + inv_m[1][1]*n[1] + inv_m[2][1]*n[2],
                        inv_m[0][2]*n[0] + inv_m[1][2]*n[1] + inv_m[2][2]*n[2]);
        }

        // True if the transform preserves angles (rotation, translation and
        // uniform scale only), which keeps solid angle densities unchanged.
        bool is_similarity() const {
            vec3 c0(m[0][0], m[1][0], m[2][0]), c1(m[0][1], m[1][1], m[2][1]), c2(m[0][2], m[1][2], m[2][2]);
            double l = c0.length_squared();
            auto close = [l](double x, double y) { return std::fabs(x - y) <= 1e-9 * std::fmax(1.0, l); };
            return close(c1.length_squared(), l) && close(c2.length_squared(), l)
                && close(dot(c0, c1), 0) && close(dot(c1, c2), 0) && close(dot(c0, c2), 0);
        }

    private:
        double m[3][3];
        vec3 t;
        double inv_m[3][3];
        vec3 inv_t;

        static vec3 mul(const double a[3][3], const vec3& v) {
            return vec3(a[0][0]*v[0] + a[0][1]*v[1] + a[0][2]*v[2],
                        a[1][0]*v[0] + a[1][1]*v[1] + a[1][2]*v[2],
                        a[2][0]*v[0] + a[2][1]*v[1] + a[2][2]*v[2]);
        }

        static void m_mul(const double a[3][3], const double b[3][3], double out[3][3]) {
            double r[3][3];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    r[i][j] = a[i][0]*b[0][j] + a[i][1]*b[1][j] + a[i][2]*b[2][j];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    out[i][j] = r[i][j];
        }
};

#endif
