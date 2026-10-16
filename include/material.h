#ifndef MATERIAL_H
#define MATERIAL_H

#include "hittable.h"
#include "color3.h"
#include "pdf.h"

#include <memory>

// What a material reports when a ray scatters off it.
class scatter_record{
    public:
        color attenuation;             // Specular: weight of skip_pdf_ray
        std::shared_ptr<pdf> pdf_ptr;  // Non-specular: density to sample directions from
        bool skip_pdf;                 // True for perfectly specular (delta) scattering
        ray skip_pdf_ray;
};

class material{
    public:
        virtual ~material() = default;

        // Returns false if the ray is absorbed. Otherwise fills srec: either a
        // specular ray (skip_pdf) or a pdf to sample the next direction from.
        virtual bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const {
            (void)r_in; (void)rec; (void)srec;
            return false;
        }

        // The BSDF times the cosine term, f(wi, wo) * |cos(theta)|, for a
        // non-specular scattering direction.
        virtual color eval(const ray& r_in, const hit_record& rec, const vec3& direction) const {
            (void)r_in; (void)rec; (void)direction;
            return color(0, 0, 0);
        }

        // The cosine-weighted part of the BSDF, as a density over directions.
        virtual double scattering_pdf(const ray& r_in, const hit_record& rec, const ray& scattered) const {
            (void)r_in; (void)rec; (void)scattered;
            return 0;
        }

        // Light emitted at the hit point; black for everything but lights.
        virtual color emitted(const ray& r_in, const hit_record& rec) const {
            (void)r_in; (void)rec;
            return color(0, 0, 0);
        }
};

class lambertian : public material{
    public:
        lambertian(const color& albedo) : albedo(albedo) {}

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            (void)r_in;
            srec.attenuation = albedo;
            srec.pdf_ptr = std::make_shared<cosine_pdf>(rec.normal);
            srec.skip_pdf = false;
            return true;
        }

        // albedo / pi * cos(theta)
        color eval(const ray& r_in, const hit_record& rec, const vec3& direction) const override {
            return albedo * scattering_pdf(r_in, rec, ray(rec.p, direction));
        }

        double scattering_pdf(const ray& r_in, const hit_record& rec, const ray& scattered)
        const override {
            (void)r_in;
            auto cos_theta = dot(rec.normal, unit_vector(scattered.direction()));
            return cos_theta < 0 ? 0 : cos_theta/pi;
        }

    private:
        color albedo;
};

class metal : public material{
    public:
        metal(const color& albedo, double fuzz) : albedo(albedo), fuzz(fuzz < 1 ? fuzz : 1) {}

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            vec3 reflected = reflect(r_in.direction(), rec.normal);
            reflected = unit_vector(reflected) + (fuzz * random_unit_vector());

            // Fuzzed rays that end up below the surface are absorbed
            if (dot(reflected, rec.normal) <= 0)
                return false;

            srec.attenuation = albedo;
            srec.pdf_ptr = nullptr;
            srec.skip_pdf = true;
            srec.skip_pdf_ray = ray(rec.p, reflected);
            return true;
        }

    private:
        color albedo;
        double fuzz;
};

class dielectric : public material{
    public:
        dielectric(double refraction_index) : refraction_index(refraction_index) {}

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            srec.attenuation = color(1.0, 1.0, 1.0);
            srec.pdf_ptr = nullptr;
            srec.skip_pdf = true;
            double ri = rec.front_face ? (1.0/refraction_index) : refraction_index;

            vec3 unit_direction = unit_vector(r_in.direction());
            double cos_theta = std::fmin(dot(-unit_direction, rec.normal), 1.0);
            double sin_theta = std::sqrt(1.0 - cos_theta*cos_theta);

            bool cannot_refract = ri * sin_theta > 1.0;  // Total internal reflection
            vec3 direction;

            if (cannot_refract || reflectance(cos_theta, ri) > random_double())
                direction = reflect(unit_direction, rec.normal);
            else
                direction = refract(unit_direction, rec.normal, ri);

            srec.skip_pdf_ray = ray(rec.p, direction);
            return true;
        }

    private:
        // Refractive index in vacuum or air, or the ratio of the material's
        // refractive index over the refractive index of the enclosing media
        double refraction_index;

        // Schlick's approximation for reflectance.
        static double reflectance(double cosine, double refraction_index) {
            auto r0 = (1 - refraction_index) / (1 + refraction_index);
            r0 = r0*r0;
            return r0 + (1-r0)*std::pow((1 - cosine), 5);
        }
};

// An area light: emits a constant radiance from its front face and scatters nothing.
class diffuse_light : public material{
    public:
        diffuse_light(const color& emit) : emit(emit) {}

        color emitted(const ray& r_in, const hit_record& rec) const override {
            (void)r_in;
            if (!rec.front_face)
                return color(0, 0, 0);
            return emit;
        }

    private:
        color emit;
};

#endif
