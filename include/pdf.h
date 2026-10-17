#ifndef PDF_H
#define PDF_H

#include "hittable.h"
#include "onb.h"

#include <algorithm>
#include <memory>

// A probability density over directions (solid angle measure), with a way to
// draw directions from it.
class pdf{
    public:
        virtual ~pdf() {}

        virtual double value(const vec3& direction) const = 0;
        virtual vec3 generate() const = 0;
};

// Uniform over the whole sphere of directions.
class sphere_pdf : public pdf{
    public:
        sphere_pdf() {}

        double value(const vec3& direction) const override {
            (void)direction;
            return 1 / (4 * pi);
        }

        vec3 generate() const override {
            return random_unit_vector();
        }
};

// Cosine-weighted over the hemisphere around w: cos(theta) / pi.
class cosine_pdf : public pdf{
    public:
        cosine_pdf(const vec3& w) : uvw(w) {}

        double value(const vec3& direction) const override {
            auto cosine_theta = dot(unit_vector(direction), uvw.w());
            return std::fmax(0, cosine_theta / pi);
        }

        vec3 generate() const override {
            return uvw.transform(random_cosine_direction());
        }

    private:
        onb uvw;
};

// Henyey-Greenstein phase function around the propagation direction w:
// p(cos) = (1 - g^2) / (4 pi (1 + g^2 - 2 g cos)^(3/2)), where cos is measured
// between w and the scattered direction. g > 0 scatters mostly forward (haze,
// clouds), g < 0 mostly backward, g = 0 is isotropic: 1 / (4 pi).
class henyey_greenstein_pdf : public pdf{
    public:
        henyey_greenstein_pdf(const vec3& w, double g) : uvw(w), g(g) {}

        double value(const vec3& direction) const override {
            return phase(dot(unit_vector(direction), uvw.w()), g);
        }

        // Inverts the cdf of cos (pbrt's formula, with cos measured from w)
        vec3 generate() const override {
            auto xi = random_double();
            double cos_theta;
            if (std::fabs(g) < 1e-3) {
                cos_theta = 1 - 2 * xi;
            } else {
                auto s = (1 - g * g) / (1 - g + 2 * g * xi);
                cos_theta = (1 + g * g - s * s) / (2 * g);
            }
            cos_theta = std::clamp(cos_theta, -1.0, 1.0);
            auto sin_theta = std::sqrt(1 - cos_theta * cos_theta);
            auto phi = 2 * pi * random_double();
            return uvw.transform(vec3(std::cos(phi) * sin_theta, std::sin(phi) * sin_theta, cos_theta));
        }

        static double phase(double cos_theta, double g) {
            auto denom = 1 + g * g - 2 * g * cos_theta;
            return (1 - g * g) / (4 * pi * denom * std::sqrt(denom));
        }

    private:
        onb uvw;
        double g;
};

// Directions from origin towards points sampled on a hittable (e.g. a light).
class hittable_pdf : public pdf{
    public:
        hittable_pdf(const hittable& objects, const point3& origin)
            : objects(objects), origin(origin) {}

        double value(const vec3& direction) const override {
            return objects.pdf_value(origin, direction);
        }

        vec3 generate() const override {
            return objects.random(origin);
        }

    private:
        const hittable& objects;
        point3 origin;
};

// Picks p0 with probability weight0 (one half by default), otherwise p1.
class mixture_pdf : public pdf{
    public:
        mixture_pdf(std::shared_ptr<pdf> p0, std::shared_ptr<pdf> p1, double weight0 = 0.5)
            : weight0(weight0)
        {
            p[0] = p0;
            p[1] = p1;
        }

        double value(const vec3& direction) const override {
            return weight0 * p[0]->value(direction) + (1 - weight0) * p[1]->value(direction);
        }

        vec3 generate() const override {
            if (random_double() < weight0)
                return p[0]->generate();
            else
                return p[1]->generate();
        }

    private:
        std::shared_ptr<pdf> p[2];
        double weight0;
};

#endif
