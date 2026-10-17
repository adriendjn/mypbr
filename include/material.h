#ifndef MATERIAL_H
#define MATERIAL_H

#include "hittable.h"
#include "color3.h"
#include "microfacet.h"
#include "pdf.h"
#include "texture.h"

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

        // Radiance leaving the front face, averaged over the surface. Only an
        // estimate, used to pick lights in proportion to their power.
        virtual color average_emission() const { return color(0, 0, 0); }
};

class lambertian : public material{
    public:
        lambertian(const color& albedo) : tex(std::make_shared<solid_color>(albedo)) {}
        lambertian(std::shared_ptr<texture> tex) : tex(tex) {}

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            (void)r_in;
            srec.attenuation = tex->value(rec.u, rec.v, rec.p);
            srec.pdf_ptr = std::make_shared<cosine_pdf>(rec.normal);
            srec.skip_pdf = false;
            return true;
        }

        // albedo / pi * cos(theta)
        color eval(const ray& r_in, const hit_record& rec, const vec3& direction) const override {
            return tex->value(rec.u, rec.v, rec.p) * scattering_pdf(r_in, rec, ray(rec.p, direction));
        }

        double scattering_pdf(const ray& r_in, const hit_record& rec, const ray& scattered)
        const override {
            (void)r_in;
            auto cos_theta = dot(rec.normal, unit_vector(scattered.direction()));
            return cos_theta < 0 ? 0 : cos_theta/pi;
        }

    private:
        std::shared_ptr<texture> tex;
};

// Conductor with a GGX microfacet BRDF. Roughness 0 is a perfect mirror
// (handled as specular); larger values blur the reflection. Fresnel uses
// Schlick's approximation with the albedo as the normal-incidence color.
class metal : public material{
    public:
        metal(const color& albedo, double roughness)
            : albedo(albedo), roughness(std::clamp(roughness, 0.0, 1.0)),
              alpha(roughness_to_alpha(this->roughness)) {}

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            if (roughness == 0) {
                srec.attenuation = albedo;
                srec.pdf_ptr = nullptr;
                srec.skip_pdf = true;
                srec.skip_pdf_ray = ray(rec.p, reflect(unit_vector(r_in.direction()), rec.normal));
                return true;
            }
            srec.attenuation = albedo;
            srec.pdf_ptr = std::make_shared<ggx_reflection_pdf>(rec.normal, -r_in.direction(), alpha);
            srec.skip_pdf = false;
            return true;
        }

        // D * G * F / (4 cos_o): the BRDF D G F / (4 cos_o cos_i) times cos_i.
        color eval(const ray& r_in, const hit_record& rec, const vec3& direction) const override {
            onb frame(rec.normal);
            vec3 wo = unit_vector(frame.to_local(-r_in.direction()));
            vec3 wi = unit_vector(frame.to_local(direction));
            if (wo.z() <= 0 || wi.z() <= 0)
                return color(0, 0, 0);
            vec3 h = unit_vector(wo + wi);
            ggx dist(alpha);
            return fresnel_schlick(dot(wi, h), albedo) * (dist.D(h) * dist.G(wo, wi) / (4 * wo.z()));
        }

    private:
        color albedo;
        double roughness;
        double alpha;
};

// Glass-like material. With roughness 0 it is perfectly smooth (specular
// reflection and refraction); otherwise it uses GGX microfacet reflection and
// transmission.
class dielectric : public material{
    public:
        dielectric(double refraction_index, double roughness = 0)
            : refraction_index(refraction_index), roughness(std::clamp(roughness, 0.0, 1.0)),
              alpha(roughness_to_alpha(this->roughness)) {}

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            if (roughness > 0) {
                srec.attenuation = color(1.0, 1.0, 1.0);
                srec.pdf_ptr = std::make_shared<rough_dielectric_pdf>(rough_bsdf(r_in, rec));
                srec.skip_pdf = false;
                return true;
            }

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

        color eval(const ray& r_in, const hit_record& rec, const vec3& direction) const override {
            if (roughness == 0)
                return color(0, 0, 0);
            auto f = rough_bsdf(r_in, rec).eval(direction);
            return color(f, f, f);
        }

    private:
        // Refractive index in vacuum or air, or the ratio of the material's
        // refractive index over the refractive index of the enclosing media
        double refraction_index;
        double roughness;
        double alpha;

        rough_dielectric_bsdf rough_bsdf(const ray& r_in, const hit_record& rec) const {
            // rec.normal faces the incoming ray; eta is far side over near side
            double eta = rec.front_face ? refraction_index : 1.0 / refraction_index;
            return rough_dielectric_bsdf(rec.normal, -r_in.direction(), eta, alpha);
        }

        // Schlick's approximation for reflectance.
        static double reflectance(double cosine, double refraction_index) {
            auto r0 = (1 - refraction_index) / (1 + refraction_index);
            r0 = r0*r0;
            return r0 + (1-r0)*std::pow((1 - cosine), 5);
        }
};

// A "standard surface" style material: a diffuse base under a GGX specular
// layer, blended towards a pure conductor by metallic.
//
// The specular layer's normal-incidence reflectance is that of a dielectric
// with the given ior (about 4% for 1.5), or the base color when metallic.
// The diffuse base only receives the light the specular layer lets through,
// on the way in and on the way out, which keeps the total reflectance at or
// below one (checked by the furnace test).
class principled : public material{
    public:
        principled(std::shared_ptr<texture> base_color, double metallic, double roughness, double ior)
            : base_color(base_color), metallic(std::clamp(metallic, 0.0, 1.0)),
              alpha(roughness_to_alpha(std::clamp(roughness, 0.0, 1.0)))
        {
            double r0 = (ior - 1) / (ior + 1);
            dielectric_f0 = r0 * r0;
        }

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            color base = base_color->value(rec.u, rec.v, rec.p);
            double cos_o = std::fmax(0.0, dot(rec.normal, unit_vector(-r_in.direction())));

            // Pick the specular lobe in proportion to its estimated share of the reflected light
            double spec_weight = average(fresnel_schlick(cos_o, specular_f0(base)));
            double diff_weight = (1 - metallic) * average(base) * (1 - fresnel_schlick(cos_o, dielectric_f0));
            double p_spec = spec_weight + diff_weight > 0 ? spec_weight / (spec_weight + diff_weight) : 1;
            p_spec = std::clamp(p_spec, 0.1, 1.0);

            auto spec = std::make_shared<ggx_reflection_pdf>(rec.normal, -r_in.direction(), alpha);
            auto diff = std::make_shared<cosine_pdf>(rec.normal);
            srec.attenuation = base;
            srec.pdf_ptr = std::make_shared<mixture_pdf>(spec, diff, p_spec);
            srec.skip_pdf = false;
            return true;
        }

        color eval(const ray& r_in, const hit_record& rec, const vec3& direction) const override {
            onb frame(rec.normal);
            vec3 wo = unit_vector(frame.to_local(-r_in.direction()));
            vec3 wi = unit_vector(frame.to_local(direction));
            if (wo.z() <= 0 || wi.z() <= 0)
                return color(0, 0, 0);

            color base = base_color->value(rec.u, rec.v, rec.p);
            vec3 h = unit_vector(wo + wi);
            ggx dist(alpha);
            color specular = fresnel_schlick(dot(wi, h), specular_f0(base))
                           * (dist.D(h) * dist.G(wo, wi) / (4 * wo.z()));

            double transmitted = (1 - fresnel_schlick(wi.z(), dielectric_f0))
                               * (1 - fresnel_schlick(wo.z(), dielectric_f0));
            color diffuse = ((1 - metallic) * transmitted * wi.z() / pi) * base;

            return specular + diffuse;
        }

    private:
        std::shared_ptr<texture> base_color;
        double metallic;
        double alpha;
        double dielectric_f0;

        color specular_f0(const color& base) const {
            return (1 - metallic) * color(dielectric_f0, dielectric_f0, dielectric_f0) + metallic * base;
        }

        static double average(const color& c) { return (c.x() + c.y() + c.z()) / 3; }
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

        color average_emission() const override { return emit; }

    private:
        color emit;
};

#endif
