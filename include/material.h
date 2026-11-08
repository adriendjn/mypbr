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

        // Radiance leaving the front face, averaged over the surface. Only an
        // estimate, used to pick lights in proportion to their power.
        virtual color average_emission() const { return color(0, 0, 0); }

        // True for smooth glass: with camera::transparent_shadows, shadow rays
        // go straight through it (see camera.h).
        virtual bool lets_shadows_through() const { return false; }

        // True for the phase function of a participating medium, whose
        // scattering events camera::max_scatter counts apart from surface bounces.
        virtual bool is_volume() const { return false; }
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

        bool lets_shadows_through() const override { return roughness == 0; }

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

            // Exact Fresnel, as rough glass uses. Schlick's approximation with
            // the incident angle stays near 4% inside the glass just before the
            // critical angle, where the real reflectance climbs to 100%, so the
            // rims of glass objects came out too dark (found by the Mitsuba
            // comparison in tools/mitsuba).
            if (cannot_refract || fresnel_dielectric(cos_theta, 1.0 / ri) > random_double())
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
};

// The shading normal a tangent-space normal map gives at rec: an RGB texture
// (loaded as linear data) whose texel c encodes the normal 2c - 1 in the
// frame (T, B, N), where N is the surface's outward normal, T is dp/du made
// perpendicular to N (Gram-Schmidt) and B = N x T, flipped to follow dp/dv
// where the uvs are mirrored. Red is along +u, green along +v (the OpenGL
// convention, v = 0 at the bottom of the image) and blue out of the surface.
// The result is on the incoming ray's side, like rec.normal.
//
// Seen at a grazing angle, a texel tilted away from the viewer would put wo
// under its own surface, where a BSDF is black (dark speckles along
// silhouettes). The normal is then bent towards wo just enough to make
// dot(n, wo) = min_cos_o (before normalizing), a small rotation in the plane
// of n and wo that leaves every other texel alone.
//
// Surfaces without tangents (dpdu zero, e.g. sphere poles or degenerate uvs)
// keep rec.normal.
inline vec3 normal_from_map(const ray& r_in, const hit_record& rec, const texture& normal_map) {
    // Smallest cosine allowed between the shading normal and wo
    constexpr double min_cos_o = 0.01;

    vec3 n = rec.front_face ? rec.normal : -rec.normal;  // Outward
    vec3 t = rec.dpdu - dot(rec.dpdu, n) * n;
    // Relative test: dp/du scales with the object, from millimeters to kilometers
    if (t.length_squared() <= 1e-12 * rec.dpdu.length_squared())
        return rec.normal;
    t = unit_vector(t);
    vec3 b = cross(n, t);
    if (dot(b, rec.dpdv) < 0)
        b = -b;  // Mirrored uvs: v runs the other way

    color c = normal_map.value(rec.u, rec.v, rec.p);
    // Blue below 0.5 would point into the surface: keep it just above
    double nz = std::fmax(2 * c.z() - 1, 1e-3);
    vec3 m = unit_vector((2 * c.x() - 1) * t + (2 * c.y() - 1) * b + nz * n);
    if (!rec.front_face)
        m = -m;  // Back to the incoming ray's side

    vec3 wo = unit_vector(-r_in.direction());
    double cos_o = dot(m, wo);
    if (cos_o < min_cos_o)
        m = unit_vector(m + (min_cos_o - cos_o) * wo);
    return m;
}

// A "standard surface" style material: a diffuse base under a GGX specular
// layer, blended towards a pure conductor by metallic, with an optional
// clear coat on top.
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
//
// Clear coat (set_clearcoat): a thin layer of varnish, as on car paint or
// lacquered wood, with its own roughness (usually near 0, a sharp
// reflection) and its own normal. It is a dielectric of ior 1.5 (f0 = 4%),
// colorless, of weight w in [0, 1] (glTF's KHR_materials_clearcoat):
//
//   f cos_i = w F_c(i.h) D G / (4 cos_o)                  coat, GGX, normal n_c
//           + (1 - w F_c(cos_o)) (1 - w F_c(cos_i)) f_base cos_i
//
// Like the diffuse base under the specular layer, everything below only gets
// the light the coat lets through, once on the way in and once on the way
// out, so adding a coat never adds energy: at normal incidence it takes 4%
// of the light into its own sharp lobe and leaves the rest 92% (96% twice),
// and at grazing angles F_c climbs and the coat takes over (the bright sky
// reflection along a car's flanks). Light that bounces between the coat and
// the base is dropped, a few percent at most.
//
// Normal maps (set_normal_map, set_clearcoat's coat_normals) bend the base
// and the coat apart, which a normal_mapped wrapper can't: wrapping would
// bend both. As in normal_mapped, a direction below the unperturbed surface
// (rec.normal) is black, and the pdf stays consistent with eval for MIS.
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

        // A tangent-space normal map for the base (not the coat). nullptr removes it.
        void set_normal_map(std::shared_ptr<texture> normals) { normal_map = normals; }

        // Adds a clear coat of weight w and roughness (red channels, clamped
        // to [0, 1]) and an optional normal map of its own. A null weight
        // removes it.
        void set_clearcoat(std::shared_ptr<texture> weight, std::shared_ptr<texture> roughness,
                           std::shared_ptr<texture> coat_normals = nullptr) {
            coat_map = weight;
            coat_roughness_map = roughness ? roughness : std::make_shared<solid_color>(color(0, 0, 0));
            coat_normal_map = coat_normals;
        }

        // Tints the coat (a candy or tinted lacquer over the base): tint is
        // what the coat lets through, in and back out, seen head-on. Light
        // crosses the layer along the refracted direction, a path 1 / cos_t
        // times its thickness each way, so by Beer-Lambert the base below is
        // scaled by tint^((1/cos_t,i + 1/cos_t,o) / 2): deeper and more
        // saturated at grazing angles. The coat's own reflection stays white.
        void set_clearcoat_color(const color& tint) { coat_tint = tint; }

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            vec3 wo = unit_vector(-r_in.direction());
            vec3 n_base = normal_map ? normal_from_map(r_in, rec, *normal_map) : rec.normal;
            double metallic = metallic_at(rec), alpha = alpha_at(rec);
            color base = base_color->value(rec.u, rec.v, rec.p);
            double cos_o = std::fmax(0.0, dot(n_base, wo));

            // Pick the specular lobe in proportion to its estimated share of the reflected light
            double spec_weight = average(fresnel_schlick(cos_o, specular_f0(base, metallic)));
            double diff_weight = (1 - metallic) * average(base) * (1 - fresnel_schlick(cos_o, dielectric_f0));
            // The near-diffuse multiple-scattering lobe is left to the cosine pdf as well
            diff_weight += ggx_ms_albedo(cos_o, alpha) * average(ggx_ms_fresnel(specular_f0(base, metallic), alpha));
            double p_spec = spec_weight + diff_weight > 0 ? spec_weight / (spec_weight + diff_weight) : 1;
            p_spec = std::clamp(p_spec, 0.1, 1.0);

            auto spec = std::make_shared<ggx_reflection_pdf>(n_base, wo, alpha);
            auto diff = std::make_shared<cosine_pdf>(n_base);
            std::shared_ptr<pdf> base_pdf = std::make_shared<mixture_pdf>(spec, diff, p_spec);
            srec.attenuation = base;
            srec.pdf_ptr = base_pdf;
            srec.skip_pdf = false;

            double w = coat_at(rec);
            if (w > 0) {
                // The coat's sharp lobe gets its share of the samples: w F_c of the
                // light against what the coat lets through to the layers below
                vec3 n_coat = coat_normal(r_in, rec);
                double coat_weight = w * fresnel_schlick(dot(n_coat, wo), coat_f0);
                double below = (1 - coat_weight) * (spec_weight + diff_weight) * average(coat_transmittance(w, 1, 1));
                double p_coat = std::clamp(coat_weight / (coat_weight + below), 0.1, 0.9);
                auto coat = std::make_shared<ggx_reflection_pdf>(n_coat, wo, coat_alpha_at(rec));
                srec.pdf_ptr = std::make_shared<mixture_pdf>(coat, base_pdf, p_coat);
            }
            return true;
        }

        color eval(const ray& r_in, const hit_record& rec, const vec3& direction) const override {
            // rec.normal is the unperturbed normal, on the side of the incoming ray:
            // a bent normal must not let light leak through the surface
            if (dot(direction, rec.normal) <= 0)
                return color(0, 0, 0);
            vec3 wo = unit_vector(-r_in.direction());
            vec3 wi = unit_vector(direction);
            vec3 n_base = normal_map ? normal_from_map(r_in, rec, *normal_map) : rec.normal;
            color below = base_eval(rec, n_base, wo, wi);

            double w = coat_at(rec);
            if (w <= 0)
                return below;

            // The coat: a colorless GGX reflection, and what it lets through twice
            vec3 n_coat = coat_normal(r_in, rec);
            double cos_o = dot(n_coat, wo), cos_i = dot(n_coat, wi);
            double through = (1 - w * fresnel_schlick(cos_o, coat_f0)) * (1 - w * fresnel_schlick(cos_i, coat_f0));
            color result = through * coat_transmittance(w, cos_o, cos_i) * below;
            if (cos_o > 0 && cos_i > 0) {
                onb frame(n_coat);
                vec3 lo = unit_vector(frame.to_local(wo)), li = unit_vector(frame.to_local(wi));
                vec3 h = unit_vector(lo + li);
                double alpha = coat_alpha_at(rec);
                ggx dist(alpha);
                double coat = fresnel_schlick(dot(li, h), coat_f0) * dist.D(h) * dist.G(lo, li) / (4 * lo.z());
                coat += ggx_ms_term(lo.z(), li.z(), alpha, color(coat_f0, coat_f0, coat_f0)).x();
                result += color(w * coat, w * coat, w * coat);
            }
            return result;
        }

    private:
        std::shared_ptr<texture> base_color;
        std::shared_ptr<texture> metallic_map;
        std::shared_ptr<texture> roughness_map;
        std::shared_ptr<texture> normal_map;          // Base only; may be null
        std::shared_ptr<texture> coat_map;            // Coat weight; null: no coat
        std::shared_ptr<texture> coat_roughness_map;
        std::shared_ptr<texture> coat_normal_map;     // May be null
        double dielectric_f0;
        color coat_tint{1, 1, 1};                     // Head-on, in and out; white: clear

        static constexpr double coat_f0 = 0.04;  // ior 1.5

        // What a coat of weight w lets through to the base and back, for
        // cosines cos_o and cos_i to the coat's normal (see set_clearcoat_color)
        color coat_transmittance(double w, double cos_o, double cos_i) const {
            if (coat_tint.x() == 1 && coat_tint.y() == 1 && coat_tint.z() == 1)
                return color(1, 1, 1);
            // Snell into ior 1.5: cos_t >= 0.745 even at grazing incidence
            auto cos_t = [](double c) {
                c = std::clamp(c, 0.0, 1.0);
                return std::sqrt(1 - (1 - c * c) / (1.5 * 1.5));
            };
            double e = (1 / cos_t(cos_o) + 1 / cos_t(cos_i)) / 2;
            color t(std::pow(coat_tint.x(), e), std::pow(coat_tint.y(), e), std::pow(coat_tint.z(), e));
            return (1 - w) * color(1, 1, 1) + w * t;
        }

        // The base layers (specular over diffuse) with normal n, for unit
        // directions wo and wi (world space)
        color base_eval(const hit_record& rec, const vec3& n, const vec3& wo_world, const vec3& wi_world) const {
            onb frame(n);
            vec3 wo = unit_vector(frame.to_local(wo_world));
            vec3 wi = unit_vector(frame.to_local(wi_world));
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

        double metallic_at(const hit_record& rec) const {
            return std::clamp(metallic_map->value(rec.u, rec.v, rec.p).x(), 0.0, 1.0);
        }

        // GGX alpha at the hit, from the roughness there
        double alpha_at(const hit_record& rec) const {
            return roughness_to_alpha(std::clamp(roughness_map->value(rec.u, rec.v, rec.p).x(), 0.0, 1.0));
        }

        double coat_at(const hit_record& rec) const {
            return coat_map ? std::clamp(coat_map->value(rec.u, rec.v, rec.p).x(), 0.0, 1.0) : 0.0;
        }

        double coat_alpha_at(const hit_record& rec) const {
            return roughness_to_alpha(std::clamp(coat_roughness_map->value(rec.u, rec.v, rec.p).x(), 0.0, 1.0));
        }

        vec3 coat_normal(const ray& r_in, const hit_record& rec) const {
            return coat_normal_map ? normal_from_map(r_in, rec, *coat_normal_map) : rec.normal;
        }

        color specular_f0(const color& base, double metallic) const {
            return (1 - metallic) * color(dielectric_f0, dielectric_f0, dielectric_f0) + metallic * base;
        }

        static double average(const color& c) { return (c.x() + c.y() + c.z()) / 3; }
};

// An area light: emits radiance from its front face and scatters nothing.
//
// By default the radiance is the same in every direction (a bare diffuser).
// A spread below 180 degrees puts a honeycomb grid in front of it, as on a
// photographer's softbox: each cell is a short tube, and seen at an angle
// theta from the normal its walls hide part of the diffuser behind it. For
// cells as deep as tan(spread / 2) times their width, the visible part falls
// linearly with tan(theta),
//     L(theta) = L0 * max(0, 1 - tan(theta) / tan(spread / 2)),
// and nothing gets out beyond spread / 2. The light stays where it is aimed
// (the car) instead of washing the backdrop, and the panel looks darker from
// the side, like a real gridded box. Light sampling picks points by area and
// never looks at emission, so it stays unbiased; only emitted() and the
// power used to pick lights change.
class diffuse_light : public material{
    public:
        diffuse_light(const color& emit, double spread_degrees = 180)
            : emit(emit), spread(spread_degrees)
        {
            double half = degrees_to_radians(spread_degrees) / 2;
            tan_half = spread_degrees >= 180 ? infinity : std::tan(half);
            cosine_share = directional_share(half);
        }

        color emitted(const ray& r_in, const hit_record& rec) const override {
            if (!rec.front_face)
                return color(0, 0, 0);
            if (spread >= 180)
                return emit;
            return emit * grid_factor(-dot(unit_vector(r_in.direction()), rec.normal));
        }

        // The cosine-weighted mean of the radiance over the hemisphere, so that
        // pi * area * this is still the emitted power.
        color average_emission() const override { return emit * cosine_share; }

        // L(theta) / L0 for cos(theta) = cos_theta.
        double grid_factor(double cos_theta) const {
            if (cos_theta <= 0)
                return 0;
            if (spread >= 180)
                return 1;
            double tan_theta = std::sqrt(std::fmax(0.0, 1 - cos_theta * cos_theta)) / cos_theta;
            return std::fmax(0.0, 1 - tan_theta / tan_half);
        }

        // (1/pi) * integral over the hemisphere of grid_factor * cos(theta):
        // 2 * integral_0^S (1 - tan t / tan S) cos t sin t dt
        //   = sin^2 S - (S - sin S cos S) / tan S,  1 for S = 90 degrees.
        static double directional_share(double half_angle) {
            if (half_angle >= pi / 2 - 1e-9)
                return 1;
            double s = std::sin(half_angle), c = std::cos(half_angle);
            return s * s - (half_angle - s * c) * c / s;
        }

    private:
        color  emit;
        double spread;        // Full cone angle in degrees; 180 = no grid
        double tan_half;
        double cosine_share;  // directional_share(spread / 2)
};

// Wraps a material with a tangent-space normal map (see normal_from_map for
// the encoding and the grazing-view fix). The inner material sees the
// perturbed normal as rec.normal; the geometry, front_face and the hit point
// are unchanged, and so is light sampling, which never looks at materials.
// Meant for the reflective materials (lambertian, metal); principled bends
// its own normals (set_normal_map), so that a coat can keep its own.
//
// A direction above the shading normal but below the unperturbed one (the
// geometric normal, or the interpolated one on smooth meshes) would leak
// light through the surface. eval() returns black there, for light samples
// and BSDF samples alike, which is the BSDF times an indicator: still
// consistent with the inner pdf for MIS, just not normalized to it. A perfect
// mirror's delta ray that goes below is mirrored back above the real surface
// instead (keeps the energy; slightly biased direction).
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

        // A copy of rec with the normal map applied to its normal.
        hit_record perturbed(const ray& r_in, const hit_record& rec) const {
            hit_record shaded = rec;
            shaded.normal = normal_from_map(r_in, rec, *normal_map);
            return shaded;
        }
};

// A thin sheet of glass, such as a car window modelled as one layer of
// triangles: light is reflected or goes straight through, never bent, since
// the two faces of a sheet bend it back. With R the Fresnel reflectance of one
// face, light bouncing between the two faces adds up to R + T^2 R / (1 - R^2)
// = 2R / (1 + R) reflected, and the rest transmitted (glTF's thin-walled
// KHR_materials_transmission). tint colors the light that passes through.
// Both lobes are delta, so the sheet is skip_pdf like smooth glass.
class thin_dielectric : public material{
    public:
        thin_dielectric(double ior, const color& tint = color(1, 1, 1)) : ior(ior), tint(tint) {}

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            vec3 unit_direction = unit_vector(r_in.direction());
            double cos_theta = std::fmin(dot(-unit_direction, rec.normal), 1.0);
            double r = fresnel_dielectric(cos_theta, ior);
            double reflectance = 2 * r / (1 + r);
            srec.pdf_ptr = nullptr;
            srec.skip_pdf = true;
            if (random_double() < reflectance) {
                srec.attenuation = color(1, 1, 1);
                srec.skip_pdf_ray = ray(rec.p, reflect(unit_direction, rec.normal));
            } else {
                srec.attenuation = tint;
                srec.skip_pdf_ray = ray(rec.p, unit_direction);
            }
            return true;
        }

    private:
        double ior;
        color tint;
};

// Adds textured light to a surface that otherwise behaves like inner (glTF's
// emissiveTexture: a glowing dashboard or logo on a normal material). It is
// not a light of the scene, only found by BSDF sampling, so it suits faint or
// small glowing details. Its emission gets the usual MIS weight against light
// sampling, which is slightly off where a real light lies right behind it.
class emissive_surface : public material{
    public:
        emissive_surface(std::shared_ptr<material> inner, std::shared_ptr<texture> emission)
            : inner(inner), emission(emission) {}

        bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
            return inner->scatter(r_in, rec, srec);
        }

        color eval(const ray& r_in, const hit_record& rec, const vec3& direction) const override {
            return inner->eval(r_in, rec, direction);
        }

        double scattering_pdf(const ray& r_in, const hit_record& rec, const ray& scattered) const override {
            return inner->scattering_pdf(r_in, rec, scattered);
        }

        color emitted(const ray& r_in, const hit_record& rec) const override {
            (void)r_in;
            if (!rec.front_face)
                return color(0, 0, 0);
            return emission->value(rec.u, rec.v, rec.p);
        }

    private:
        std::shared_ptr<material> inner;
        std::shared_ptr<texture> emission;
};

#endif
