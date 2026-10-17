#ifndef SUN_H
#define SUN_H

#include "color3.h"
#include "hittable.h"
#include "onb.h"

#include <cmath>

// A distant light of small angular size: the sun. Seen from anywhere in the
// scene it covers the same cone of directions around direction (towards
// the sun), so its shadows are parallel; the cone's width sets how soft they
// are, from sharp at the contact point to blurred further away.
//
// Its brightness is given as irradiance: the light falling on a surface that
// faces it, which is what sets how bright a sunlit wall is. The radiance in
// the cone is that spread over the cone's solid angle, E / omega, so a wider
// sun gives softer shadows but the same lighting.
//
// Like environment_light it is a hittable only so it can sit in the lights:
// rays never hit it, and the camera adds its radiance to rays that escape.
// Shadow rays are aimed uniformly inside the cone, density 1 / omega.
class sun_light : public hittable{
    public:
        // angular_diameter in degrees (the real sun is about 0.53).
        sun_light(const vec3& direction, double angular_diameter, const color& irradiance)
            : frame(direction)
        {
            double half = degrees_to_radians(std::fmax(angular_diameter, 1e-3)) / 2;
            cos_max = std::cos(half);
            solid_angle = 2 * pi * (1 - cos_max);
            irradiance_ = irradiance;
            radiance_in_cone = irradiance / solid_angle;
        }

        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            (void)r; (void)ray_t; (void)rec;
            return false;
        }

        aabb bounding_box() const override { return aabb(); }

        const vec3& direction() const { return frame.w(); }

        // Radiance arriving along a direction that escapes the scene.
        color radiance(const vec3& direction) const {
            return in_cone(direction) ? radiance_in_cone : color(0, 0, 0);
        }

        double pdf_value(const point3& origin, const vec3& direction) const override {
            (void)origin;
            return in_cone(direction) ? 1 / solid_angle : 0;
        }

        // Uniform in the cone: cos(theta) is uniform in [cos_max, 1]
        vec3 random(const point3& origin) const override {
            (void)origin;
            auto cos_theta = 1 - random_double() * (1 - cos_max);
            auto sin_theta = std::sqrt(std::fmax(0.0, 1 - cos_theta * cos_theta));
            auto phi = 2 * pi * random_double();
            return frame.transform(vec3(std::cos(phi) * sin_theta, std::sin(phi) * sin_theta, cos_theta));
        }

        // The power reaching a scene of the given bounding radius: irradiance
        // E through the disk of area pi R^2 the scene shows the sun, the same
        // estimate environment_light::power uses.
        double power(double scene_radius) const {
            return pi * scene_radius * scene_radius * luminance(irradiance_);
        }

    private:
        onb frame;
        double cos_max;
        double solid_angle;
        color irradiance_;
        color radiance_in_cone;

        bool in_cone(const vec3& direction) const {
            return dot(unit_vector(direction), frame.w()) >= cos_max;
        }
};

#endif
