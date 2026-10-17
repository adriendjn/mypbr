#ifndef ATMOSPHERE_H
#define ATMOSPHERE_H

#include "color3.h"
#include "vec3.h"

#include <cmath>
#include <vector>

// A physically based sky: sunlight scattered once by the air on its way to
// the viewer, computed for every direction and baked into an
// equirectangular image that environment_light then samples like any HDR
// sky.
//
// Three things in the air shape the sky:
//
// - Rayleigh scattering by the molecules themselves. Their size is far below
//   the wavelength of light, so they scatter as 1 / lambda^4: blue about
//   five times more than red. Blue light from the sun is scattered all over
//   the sky (blue sky), and a sun low on the horizon, whose light crosses
//   much more air, loses its blue and turns orange (sunsets).
// - Mie scattering by aerosols (dust, water droplets), about as large as the
//   wavelength: roughly grey, and strongly forward (Henyey-Greenstein g of
//   0.76 to 0.9), which makes the bright haze around the sun and the whitish
//   horizon.
// - Ozone, which absorbs some orange and red: it keeps twilight skies blue.
//
// Each falls off with altitude (exponentially for air and aerosols, with
// scale heights of 8 km and 1.2 km; ozone in a layer around 25 km). For a
// direction, the code marches along the view ray through the atmosphere;
// at each step, light from the sun that reached that point (attenuated on
// its way down, sun_transmittance) scatters towards the viewer and is
// attenuated again on its way back. The coefficients are those of Earth's
// atmosphere (Hillaire, "A Scalable and Production Ready Sky and Atmosphere
// Rendering Technique", 2020), scaled by the settings below.
class atmosphere_settings{
    public:
        double rayleigh = 1;   // Air density, relative to Earth's
        double mie = 1;        // Aerosol (haze) density
        double mie_g = 0.8;    // Forward scattering of the haze
        double ozone = 1;      // Ozone density
        double intensity = 3;  // Overall scale of the sky's radiance. Single
                               // scattering alone leaves the sky about three times
                               // too dark next to measured skies (light scattered
                               // more than once is missing), hence the default.
};

class atmosphere{
    public:
        explicit atmosphere(const atmosphere_settings& s) : settings(s) {}

        // Fraction of sunlight that reaches the ground along direction (the
        // sun's color through the air; 0 below the horizon).
        color sun_transmittance(const vec3& direction) const {
            return transmittance(point3(0, viewer_radius, 0), unit_vector(direction));
        }

        // Radiance of the sky in direction for a sun in sun_direction giving
        // irradiance sun_irradiance above the atmosphere. Single scattering only.
        color radiance(const vec3& direction, const vec3& sun_direction, const color& sun_irradiance) const {
            vec3 d = unit_vector(direction), s = unit_vector(sun_direction);
            point3 o(0, viewer_radius, 0);
            double t_max = distance_to_top(o, d);
            double t_ground = distance_to_ground(o, d);
            if (t_ground > 0)
                t_max = std::fmin(t_max, t_ground);

            double mu = dot(d, s);
            double phase_r = 3 / (16 * pi) * (1 + mu * mu);
            double g = settings.mie_g;
            double denom = 1 + g * g - 2 * g * mu;
            double phase_m = (1 - g * g) / (4 * pi * denom * std::sqrt(denom));

            const int steps = 32;
            double dt = t_max / steps;
            color depth(0, 0, 0);  // Optical depth from the viewer so far
            color sum(0, 0, 0);
            for (int k = 0; k < steps; k++) {
                point3 p = o + (k + 0.5) * dt * d;
                double h = p.length() - ground_radius;
                double rho_r, rho_m, rho_o;
                densities(h, rho_r, rho_m, rho_o);
                color extinction = rho_r * beta_rayleigh() + rho_m * beta_mie_extinction() + rho_o * beta_ozone();
                depth += 0.5 * dt * extinction;  // Midpoint: half a step to reach p
                color to_sun = transmittance(p, s);
                color view = exp(-1.0 * depth);
                sum += view * to_sun * (rho_r * phase_r * beta_rayleigh() + rho_m * phase_m * beta_mie()) * dt;
                depth += 0.5 * dt * extinction;
            }
            return settings.intensity * sum * sun_irradiance;
        }

        // The sky as an equirectangular image laid out like environment_light
        // reads it: row j at angle theta = pi (j + 0.5) / height from +y,
        // column i at azimuth phi = 2 pi ((i + 0.5) / width - 0.5), looking
        // down -z at phi = 0.
        std::vector<color> bake(int width, int height, const vec3& sun_direction,
                                const color& sun_irradiance) const {
            std::vector<color> pixels(size_t(width) * height);
            for (int j = 0; j < height; j++) {
                double theta = pi * (j + 0.5) / height;
                for (int i = 0; i < width; i++) {
                    double phi = 2 * pi * ((i + 0.5) / width - 0.5);
                    vec3 d(std::sin(theta) * std::sin(phi), std::cos(theta), -std::sin(theta) * std::cos(phi));
                    pixels[size_t(j) * width + i] = radiance(d, sun_direction, sun_irradiance);
                }
            }
            return pixels;
        }

    private:
        atmosphere_settings settings;

        // Earth, in meters
        static constexpr double ground_radius = 6360e3;
        static constexpr double top_radius = 6460e3;
        static constexpr double viewer_radius = ground_radius + 200;

        color beta_rayleigh() const { return settings.rayleigh * color(5.802e-6, 13.558e-6, 33.1e-6); }
        color beta_mie() const { return settings.mie * color(3.996e-6, 3.996e-6, 3.996e-6); }
        color beta_mie_extinction() const { return settings.mie * color(4.40e-6, 4.40e-6, 4.40e-6); }
        color beta_ozone() const { return settings.ozone * color(0.650e-6, 1.881e-6, 0.085e-6); }

        static void densities(double h, double& rayleigh, double& mie, double& ozone) {
            rayleigh = std::exp(-h / 8000);
            mie = std::exp(-h / 1200);
            ozone = std::fmax(0.0, 1 - std::fabs(h - 25000) / 15000);
        }

        static color exp(const color& c) {
            return color(std::exp(c.x()), std::exp(c.y()), std::exp(c.z()));
        }

        // Distance along unit d from o (inside the atmosphere) to its top.
        static double distance_to_top(const point3& o, const vec3& d) {
            double b = dot(o, d), c = o.length_squared() - top_radius * top_radius;
            return -b + std::sqrt(std::fmax(0.0, b * b - c));
        }

        // Distance to the ground, or -1 if the ray misses the planet.
        static double distance_to_ground(const point3& o, const vec3& d) {
            double b = dot(o, d), c = o.length_squared() - ground_radius * ground_radius;
            double disc = b * b - c;
            if (disc < 0)
                return -1;
            double t = -b - std::sqrt(disc);
            return t > 0 ? t : -1;
        }

        // Transmittance from p to space along unit d (0 if the ground is in the way).
        color transmittance(const point3& p, const vec3& d) const {
            if (distance_to_ground(p, d) > 0)
                return color(0, 0, 0);
            const int steps = 16;
            double dt = distance_to_top(p, d) / steps;
            color depth(0, 0, 0);
            for (int k = 0; k < steps; k++) {
                double h = (p + (k + 0.5) * dt * d).length() - ground_radius;
                double rho_r, rho_m, rho_o;
                densities(h, rho_r, rho_m, rho_o);
                depth += dt * (rho_r * beta_rayleigh() + rho_m * beta_mie_extinction() + rho_o * beta_ozone());
            }
            return exp(-1.0 * depth);
        }
};

#endif
