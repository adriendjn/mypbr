#ifndef VEC3_H
#define VEC3_H

#include "rtweekend.h"

#include <iostream>
#include <cmath>

class vec3{
    public:
        double v[3];

        vec3() : v{0.0, 0.0, 0.0} {}

        vec3(double x, double y, double z) : v{x, y, z} {}

        double x() const {return v[0];}
        double y() const {return v[1];}
        double z() const {return v[2];}

        vec3 operator -() const { return vec3(-v[0], -v[1], -v[2]);}
        double operator[](int i) const { return v[i]; }
        double& operator[](int i) { return v[i]; }

        vec3& operator += (const vec3& t){
            v[0] += t[0];
            v[1] += t[1];
            v[2] += t[2];
            return *this;
        }

        vec3& operator *= (double t){
            v[0] *= t;
            v[1] *= t;
            v[2] *= t;
            return *this;
        }

        vec3& operator /= (double t){
            v[0] /= t;
            v[1] /= t;
            v[2] /= t;
            return *this;
        }

        double length_squared() const {
            return v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        }

        double length() const {
            return std::sqrt(length_squared());
        }

        // Return true if the vector is close to zero in all dimensions.
        bool near_zero() const {
            auto s = 1e-8;
            return (std::fabs(v[0]) < s) && (std::fabs(v[1]) < s) && (std::fabs(v[2]) < s);
        }

        static vec3 random() {
            return vec3(random_double(), random_double(), random_double());
        }

        static vec3 random(double min, double max) {
            return vec3(random_double(min, max), random_double(min, max), random_double(min, max));
        }
};

using point3 = vec3;

inline std::ostream& operator<<(std::ostream& out, const vec3& v){
    return out << v.v[0] << ' ' << v.v[1] << ' ' << v.v[2];
}

inline vec3 operator+(const vec3& u, const vec3& v) {
    return vec3(u[0] + v[0], u[1] + v[1], u[2] + v[2]);
}

inline vec3 operator-(const vec3& u, const vec3& v) {
    return vec3(u[0] - v[0], u[1] - v[1], u[2] - v[2]);
}

inline vec3 operator*(const vec3& u, const vec3& v){
    return vec3(u[0] * v[0], u[1] * v[1], u[2] * v[2]);
}

inline vec3 operator*(double t, const vec3& v) {
    return vec3(t*v[0], t*v[1], t*v[2]);
}

inline vec3 operator*(const vec3& v, double t) {
    return vec3(t*v[0], t*v[1], t*v[2]);
}

inline vec3 operator/(const vec3& v, double t) {
    return (1/t) * v;
}

inline double dot(const vec3& u, const vec3& v) {
    return u[0] * v[0]
         + u[1] * v[1]
         + u[2] * v[2];
}

inline vec3 cross(const vec3& u, const vec3& v) {
    return vec3(u[1] * v[2] - u[2] * v[1],
                u[2] * v[0] - u[0] * v[2],
                u[0] * v[1] - u[1] * v[0]);
}

inline vec3 unit_vector(const vec3& v) {
    return v / v.length();
}

// Uniformly distributed direction: rejection-sample the unit ball, then normalize.
inline vec3 random_unit_vector() {
    while (true) {
        auto p = vec3::random(-1, 1);
        auto lensq = p.length_squared();
        if (1e-160 < lensq && lensq <= 1)
            return p / std::sqrt(lensq);
    }
}

// Cosine-weighted direction on the hemisphere around +z.
inline vec3 random_cosine_direction() {
    auto r1 = random_double();
    auto r2 = random_double();

    auto phi = 2*pi*r1;
    auto x = std::cos(phi) * std::sqrt(r2);
    auto y = std::sin(phi) * std::sqrt(r2);
    auto z = std::sqrt(1-r2);

    return vec3(x, y, z);
}

// Uniformly distributed point inside the unit disk in the xy plane.
inline vec3 random_in_unit_disk() {
    while (true) {
        auto p = vec3(random_double(-1, 1), random_double(-1, 1), 0);
        if (p.length_squared() < 1)
            return p;
    }
}

// Mirror reflection of v about the surface normal n.
inline vec3 reflect(const vec3& v, const vec3& n) {
    return v - 2*dot(v, n)*n;
}

// Refraction of unit vector uv through a surface with unit normal n, where
// etai_over_etat is the ratio of refractive indices (incident over transmitted).
inline vec3 refract(const vec3& uv, const vec3& n, double etai_over_etat) {
    auto cos_theta = std::fmin(dot(-uv, n), 1.0);
    vec3 r_out_perp =  etai_over_etat * (uv + cos_theta*n);
    vec3 r_out_parallel = -std::sqrt(std::fabs(1.0 - r_out_perp.length_squared())) * n;
    return r_out_perp + r_out_parallel;
}

// Uniformly distributed direction in the hemisphere around normal.
inline vec3 random_on_hemisphere(const vec3& normal) {
    vec3 on_unit_sphere = random_unit_vector();
    if (dot(on_unit_sphere, normal) > 0.0)
        return on_unit_sphere;
    else
        return -on_unit_sphere;
}

#endif
