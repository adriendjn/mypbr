#ifndef PDF_H
#define PDF_H

#include "hittable.h"
#include "onb.h"

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

// Picks either of two pdfs with probability one half each.
class mixture_pdf : public pdf{
    public:
        mixture_pdf(std::shared_ptr<pdf> p0, std::shared_ptr<pdf> p1) {
            p[0] = p0;
            p[1] = p1;
        }

        double value(const vec3& direction) const override {
            return 0.5 * p[0]->value(direction) + 0.5 * p[1]->value(direction);
        }

        vec3 generate() const override {
            if (random_double() < 0.5)
                return p[0]->generate();
            else
                return p[1]->generate();
        }

    private:
        std::shared_ptr<pdf> p[2];
};

#endif
