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

        // Fraction of the light carried along r that survives the segment
        // ending at rec. Called for every hit; materials with an absorbing
        // interior return less than 1 when the segment ran inside them, i.e.
        // when rec is a back face. Everything else is clear (1).
        virtual color interior_transmittance(const ray& r, const hit_record& rec) const {
            (void)r; (void)rec;
            return color(1, 1, 1);
        }
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
// A Kulla-Conty lobe adds back the light single-scattering GGX loses to
// bounces between microfacets, so a white metal reflects everything at any
// roughness.
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
            auto spec = std::make_shared<ggx_reflection_pdf>(rec.normal, -r_in.direction(), alpha);
            srec.skip_pdf = false;

            // The compensation lobe is near-diffuse: sample it with a cosine
            // pdf, chosen in proportion to its share of the reflected energy
            // (single scatter keeps about E(mu_o) F, the lobe adds (1 - E) F_ms)
            double cos_o = std::fmax(0.0, dot(rec.normal, unit_vector(-r_in.direction())));
            double ms_weight = ggx_ms_albedo(cos_o, alpha) * average(ggx_ms_fresnel(albedo, alpha));
            double ss_weight = ggx_albedo_table::get().E(cos_o, alpha) * average(fresnel_schlick(cos_o, albedo));
            if (ms_weight <= 0) {
                srec.pdf_ptr = spec;
                return true;
            }
            double p_spec = std::clamp(ss_weight / (ss_weight + ms_weight), 0.1, 1.0);
            srec.pdf_ptr = std::make_shared<mixture_pdf>(spec, std::make_shared<cosine_pdf>(rec.normal), p_spec);
            return true;
        }

        // D * G * F / (4 cos_o): the BRDF D G F / (4 cos_o cos_i) times cos_i,
        // plus the multiple-scattering lobe F_ms f_ms cos_i.
        color eval(const ray& r_in, const hit_record& rec, const vec3& direction) const override {
            onb frame(rec.normal);
            vec3 wo = unit_vector(frame.to_local(-r_in.direction()));
            vec3 wi = unit_vector(frame.to_local(direction));
            if (wo.z() <= 0 || wi.z() <= 0)
                return color(0, 0, 0);
            vec3 h = unit_vector(wo + wi);
            ggx dist(alpha);
            return fresnel_schlick(dot(wi, h), albedo) * (dist.D(h) * dist.G(wo, wi) / (4 * wo.z()))
                 + ggx_ms_term(wo.z(), wi.z(), alpha, albedo);
        }

    private:
        color albedo;
        double roughness;
        double alpha;

        static double average(const color& c) { return (c.x() + c.y() + c.z()) / 3; }
};

// Glass-like material. With roughness 0 it is perfectly smooth (specular
// reflection and refraction); otherwise it uses GGX microfacet reflection and
// transmission.
//
// absorption is the Beer-Lambert coefficient sigma_a (per unit length, per
// channel): light that travels a distance d inside is scaled by
// exp(-sigma_a d). 0 is perfectly clear glass. The surfaces themselves stay
// white; the color comes from the volume, so thick parts look darker and more
// saturated than thin ones.
class dielectric : public material{
    public:
        dielectric(double refraction_index, double roughness = 0, const color& absorption = color(0, 0, 0))
            : refraction_index(refraction_index), roughness(std::clamp(roughness, 0.0, 1.0)),
              alpha(roughness_to_alpha(this->roughness)), sigma_a(absorption) {}

        // The absorption that gives transmittance tint (each channel in (0, 1])
        // after a distance d: exp(-sigma_a d) = tint, so sigma_a = -ln(tint) / d.
        static color absorption_for(const color& tint, double distance) {
            return color(-std::log(tint.x()), -std::log(tint.y()), -std::log(tint.z())) / distance;
        }

        // A back face hit means the segment that just ended ran through the
        // glass, whichever way the ray got in (refraction, total internal
        // reflection, a camera inside). Its length is t |dir|: directions are
        // not unit length, and instance::hit keeps t the same in both spaces,
        // so this is the world-space distance.
        color interior_transmittance(const ray& r, const hit_record& rec) const override {
            if (rec.front_face || (sigma_a.x() == 0 && sigma_a.y() == 0 && sigma_a.z() == 0))
                return color(1, 1, 1);
            double d = rec.t * r.direction().length();
            return color(std::exp(-sigma_a.x() * d), std::exp(-sigma_a.y() * d), std::exp(-sigma_a.z() * d));
        }

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
        color sigma_a;  // Absorption coefficient of the interior

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
// below one (checked by the furnace test). The specular layer gets the same
// multiple-scattering compensation as metal; for a dielectric f0 (0.04) its
// F_ms is about F_avg^2 < 1%, which the (1 - F) factors leave room for.
//
// Metallic and roughness can vary over the surface: they are read from the
// red channel of a texture (load image maps as linear data) at every hit,
// clamped to [0, 1]. Constants are solid textures.
class principled : public material{
    public:
        principled(std::shared_ptr<texture> base_color, double metallic, double roughness, double ior)
            : principled(base_color, std::make_shared<solid_color>(color(metallic, metallic, metallic)),
                         std::make_shared<solid_color>(color(roughness, roughness, roughness)), ior) {}

        principled(std::shared_ptr<texture> base_color, std::shared_ptr<texture> metallic_map,
                   std::shared_ptr<texture> roughness_map, double ior)
            : base_color(base_color), metallic_map(metallic_map), roughness_map(roughness_map)
        {
            double r0 = (ior - 1) / (ior + 1);
            dielectric_f0 = r0 * r0;
        }

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            double metallic = metallic_at(rec), alpha = alpha_at(rec);
            color base = base_color->value(rec.u, rec.v, rec.p);
            double cos_o = std::fmax(0.0, dot(rec.normal, unit_vector(-r_in.direction())));

            // Pick the specular lobe in proportion to its estimated share of the reflected light
            double spec_weight = average(fresnel_schlick(cos_o, specular_f0(base, metallic)));
            double diff_weight = (1 - metallic) * average(base) * (1 - fresnel_schlick(cos_o, dielectric_f0));
            // The near-diffuse multiple-scattering lobe is left to the cosine pdf as well
            diff_weight += ggx_ms_albedo(cos_o, alpha) * average(ggx_ms_fresnel(specular_f0(base, metallic), alpha));
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

            double metallic = metallic_at(rec), alpha = alpha_at(rec);
            color base = base_color->value(rec.u, rec.v, rec.p);
            vec3 h = unit_vector(wo + wi);
            ggx dist(alpha);
            color specular = fresnel_schlick(dot(wi, h), specular_f0(base, metallic))
                           * (dist.D(h) * dist.G(wo, wi) / (4 * wo.z()))
                           + ggx_ms_term(wo.z(), wi.z(), alpha, specular_f0(base, metallic));

            double transmitted = (1 - fresnel_schlick(wi.z(), dielectric_f0))
                               * (1 - fresnel_schlick(wo.z(), dielectric_f0));
            color diffuse = ((1 - metallic) * transmitted * wi.z() / pi) * base;

            return specular + diffuse;
        }

    private:
        std::shared_ptr<texture> base_color;
        std::shared_ptr<texture> metallic_map;
        std::shared_ptr<texture> roughness_map;
        double dielectric_f0;

        double metallic_at(const hit_record& rec) const {
            return std::clamp(metallic_map->value(rec.u, rec.v, rec.p).x(), 0.0, 1.0);
        }

        // GGX alpha at the hit, from the roughness there
        double alpha_at(const hit_record& rec) const {
            return roughness_to_alpha(std::clamp(roughness_map->value(rec.u, rec.v, rec.p).x(), 0.0, 1.0));
        }

        color specular_f0(const color& base, double metallic) const {
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

    private:
        color emit;
};

// Wraps a material with a tangent-space normal map: an RGB texture (loaded
// as linear data) whose texel c encodes the normal 2c - 1 in the frame
// (T, B, N), where N is the surface's outward shading normal, T is dp/du made
// perpendicular to N (Gram-Schmidt) and B = N x T, flipped to follow dp/dv
// where the uvs are mirrored. Red is along +u, green along +v (the OpenGL
// convention, v = 0 at the bottom of the image) and blue out of the surface.
// The inner material sees the perturbed normal as rec.normal; the geometry,
// front_face and the hit point are unchanged, and so is light sampling,
// which never looks at materials.
//
// A bent normal can disagree with the real surface, so two edge cases are
// handled here, for the reflective materials this is meant for (lambertian,
// metal, principled):
//
//   - wo below the shading normal: seen at a grazing angle, a texel tilted
//     away from the viewer would put wo under its own surface, where a
//     BSDF is black (dark speckles along silhouettes). The normal is bent
//     towards wo just enough to make dot(n, wo) = min_cos_o (before
//     normalizing), a small rotation in the plane of n and wo that leaves
//     every other texel alone.
//   - wi below the real surface: a direction above the shading normal but
//     below the unperturbed one (the geometric normal, or the interpolated
//     one on smooth meshes) would leak light through the surface. eval()
//     returns black there, for light samples and BSDF samples alike, which
//     is the BSDF times an indicator: still consistent with the inner pdf
//     for MIS, just not normalized to it. A perfect mirror's delta ray that
//     goes below is mirrored back above the real surface instead (keeps the
//     energy; slightly biased direction).
//
// Surfaces without tangents (dpdu zero, e.g. sphere poles or degenerate
// uvs) are shaded with the unperturbed normal.
class normal_mapped : public material{
    public:
        normal_mapped(std::shared_ptr<material> inner, std::shared_ptr<texture> normal_map)
            : inner(inner), normal_map(normal_map) {}

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            hit_record shaded = perturbed(r_in, rec);
            if (!inner->scatter(r_in, shaded, srec))
                return false;
            if (srec.skip_pdf) {
                // A reflection (on the shading normal's side) that went below the real surface
                vec3 d = srec.skip_pdf_ray.direction();
                if (dot(d, shaded.normal) > 0 && dot(d, rec.normal) < 0)
                    srec.skip_pdf_ray = ray(srec.skip_pdf_ray.origin(), d - 2 * dot(d, rec.normal) * rec.normal);
            }
            return true;
        }

        color eval(const ray& r_in, const hit_record& rec, const vec3& direction) const override {
            // rec.normal is the unperturbed normal, on the side of the incoming ray
            if (dot(direction, rec.normal) <= 0)
                return color(0, 0, 0);
            return inner->eval(r_in, perturbed(r_in, rec), direction);
        }

        double scattering_pdf(const ray& r_in, const hit_record& rec, const ray& scattered) const override {
            return inner->scattering_pdf(r_in, perturbed(r_in, rec), scattered);
        }

        color emitted(const ray& r_in, const hit_record& rec) const override {
            return inner->emitted(r_in, rec);
        }

    private:
        std::shared_ptr<material> inner;
        std::shared_ptr<texture> normal_map;

        // Smallest cosine allowed between the shading normal and wo
        static constexpr double min_cos_o = 0.01;

        // A copy of rec with the normal map applied to its normal.
        hit_record perturbed(const ray& r_in, const hit_record& rec) const {
            hit_record shaded = rec;
            vec3 n = rec.front_face ? rec.normal : -rec.normal;  // Outward
            vec3 t = rec.dpdu - dot(rec.dpdu, n) * n;
            // Relative test: dp/du scales with the object, from millimeters to kilometers
            if (t.length_squared() <= 1e-12 * rec.dpdu.length_squared())
                return shaded;
            t = unit_vector(t);
            vec3 b = cross(n, t);
            if (dot(b, rec.dpdv) < 0)
                b = -b;  // Mirrored uvs: v runs the other way

            color c = normal_map->value(rec.u, rec.v, rec.p);
            // Blue below 0.5 would point into the surface: keep it just above
            double nz = std::fmax(2 * c.z() - 1, 1e-3);
            vec3 m = unit_vector((2 * c.x() - 1) * t + (2 * c.y() - 1) * b + nz * n);
            if (!rec.front_face)
                m = -m;  // Back to the incoming ray's side

            vec3 wo = unit_vector(-r_in.direction());
            double cos_o = dot(m, wo);
            if (cos_o < min_cos_o)
                m = unit_vector(m + (min_cos_o - cos_o) * wo);
            shaded.normal = m;
            return shaded;
        }
};

#endif
