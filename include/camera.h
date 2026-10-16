#ifndef CAMERA_H
#define CAMERA_H

#include "hittable.h"
#include "color3.h"
#include "hittable_list.h"
#include "material.h"

#include <iostream>
#include <vector>

class camera{
    public:
        double aspect_ratio = 1.0;  // Ratio of image width over height
        int    image_width  = 100;  // Rendered image width in pixels
        int    samples_per_pixel = 10;  // Count of random samples for each pixel
        int    max_depth         = 10;  // Hard cap on bounces; Russian roulette usually ends paths sooner

        double vfov     = 90;               // Vertical view angle (field of view)
        point3 lookfrom = point3(0, 0, 0);  // Point camera is looking from
        point3 lookat   = point3(0, 0, -1); // Point camera is looking at
        vec3   vup      = vec3(0, 1, 0);    // Camera-relative "up" direction

        bool   use_sky    = true;             // Sky gradient behind the scene, or...
        color  background = color(0, 0, 0);  // ...a constant background color

        double defocus_angle = 0;  // Variation angle of rays through each pixel
        double focus_dist    = 10; // Distance from camera lookfrom point to plane of perfect focus

        int height() const { return image_height; }

        // Renders the world and returns the linear radiance of every pixel,
        // row by row from the top left. lights are the emitters to sample
        // directly (next event estimation); they must also be part of world.
        std::vector<color> render(const hittable& world, const hittable_list& lights) {
            initialize();

            std::vector<color> pixels(size_t(image_width) * image_height);

            for (int j = 0; j < image_height; j++){
                std::clog << "\rScanlines remaining: " << (image_height - j) << ' ' << std::flush;
                for (int i = 0; i < image_width; i++)
                    pixels[size_t(j) * image_width + i] = render_pixel(i, j, world, lights);
            }
            std::clog << "\rDone.                   \n";
            return pixels;
        }

        // Computes the image height and camera basis; render() calls it.
        void initialize() {
            image_height = int(image_width / aspect_ratio);
            image_height = (image_height < 1) ? 1 : image_height;

            pixel_samples_scale = 1.0 / samples_per_pixel;
            sqrt_spp = int(std::sqrt(samples_per_pixel));
            recip_sqrt_spp = 1.0 / sqrt_spp;

            center = lookfrom;

            // Viewport dimensions
            auto theta = degrees_to_radians(vfov);
            auto h = std::tan(theta/2);
            auto viewport_height = 2 * h * focus_dist;
            auto viewport_width = viewport_height * (double(image_width)/image_height);

            // Unit basis vectors for the camera coordinate frame
            w = unit_vector(lookfrom - lookat);
            u = unit_vector(cross(vup, w));
            v = cross(w, u);

            // Vectors across the horizontal and down the vertical viewport edges
            vec3 viewport_u = viewport_width * u;
            vec3 viewport_v = viewport_height * -v;

            // Horizontal and vertical delta vectors from pixel to pixel
            pixel_delta_u = viewport_u / image_width;
            pixel_delta_v = viewport_v / image_height;

            // Location of the upper left pixel
            auto viewport_upper_left = center - (focus_dist * w) - viewport_u/2 - viewport_v/2;
            pixel00_loc = viewport_upper_left + 0.5 * (pixel_delta_u + pixel_delta_v);

            // Camera defocus disk basis vectors
            auto defocus_radius = focus_dist * std::tan(degrees_to_radians(defocus_angle / 2));
            defocus_disk_u = u * defocus_radius;
            defocus_disk_v = v * defocus_radius;
        }

    private:
        int    image_height;   // Rendered image height
        double pixel_samples_scale;  // Color scale factor for a sum of pixel samples
        int    sqrt_spp;             // Square root of number of samples per pixel
        double recip_sqrt_spp;       // 1 / sqrt_spp
        point3 center;         // Camera center
        point3 pixel00_loc;    // Location of pixel 0, 0
        vec3   pixel_delta_u;  // Offset to pixel to the right
        vec3   pixel_delta_v;  // Offset to pixel below
        vec3   u, v, w;        // Camera frame basis vectors
        vec3   defocus_disk_u; // Defocus disk horizontal radius
        vec3   defocus_disk_v; // Defocus disk vertical radius

        // Averages samples_per_pixel radiance estimates for pixel i, j. The
        // first sqrt_spp^2 samples are stratified on a jittered grid inside the
        // pixel; any remainder is placed uniformly at random.
        color render_pixel(int i, int j, const hittable& world, const hittable_list& lights) const {
            color pixel_color(0, 0, 0);
            for (int s_j = 0; s_j < sqrt_spp; s_j++)
                for (int s_i = 0; s_i < sqrt_spp; s_i++)
                    pixel_color += sample_radiance(get_ray(i, j, sample_square_stratified(s_i, s_j)), world, lights);
            for (int extra = sqrt_spp*sqrt_spp; extra < samples_per_pixel; extra++)
                pixel_color += sample_radiance(get_ray(i, j, sample_square()), world, lights);
            return pixel_samples_scale * pixel_color;
        }

        // One path's radiance, with NaN or infinite estimates dropped (counted
        // as black) so a single bad sample can't poison a pixel.
        color sample_radiance(const ray& r, const hittable& world, const hittable_list& lights) const {
            color c = ray_color(r, world, lights);
            for (int k = 0; k < 3; k++)
                if (!std::isfinite(c[k]))
                    return color(0, 0, 0);
            return c;
        }

        // Constructs a camera ray originating from the defocus disk and directed
        // at the point offset (in pixel units) from the center of pixel i, j.
        ray get_ray(int i, int j, const vec3& offset) const {
            auto pixel_sample = pixel00_loc
                              + ((i + offset.x()) * pixel_delta_u)
                              + ((j + offset.y()) * pixel_delta_v);

            auto ray_origin = (defocus_angle <= 0) ? center : defocus_disk_sample();
            return ray(ray_origin, pixel_sample - ray_origin);
        }

        // Returns a random point in the [-.5,-.5]-[+.5,+.5] unit square.
        static vec3 sample_square() {
            return vec3(random_double() - 0.5, random_double() - 0.5, 0);
        }

        // Returns a random point in the square sub-pixel (s_i, s_j) of the
        // [-.5,-.5]-[+.5,+.5] unit square.
        vec3 sample_square_stratified(int s_i, int s_j) const {
            auto px = ((s_i + random_double()) * recip_sqrt_spp) - 0.5;
            auto py = ((s_j + random_double()) * recip_sqrt_spp) - 0.5;
            return vec3(px, py, 0);
        }

        // Returns a random point in the camera defocus disk.
        point3 defocus_disk_sample() const {
            auto p = random_in_unit_disk();
            return center + (p[0] * defocus_disk_u) + (p[1] * defocus_disk_v);
        }

        // Power heuristic (beta = 2) weight for a sample drawn from the strategy
        // with density pdf_a, when pdf_b could also have produced it.
        static double power_heuristic(double pdf_a, double pdf_b) {
            auto a2 = pdf_a * pdf_a;
            auto b2 = pdf_b * pdf_b;
            return (a2 + b2) > 0 ? a2 / (a2 + b2) : 0;
        }

        // Path traced estimate of the radiance arriving along r.
        //
        // Each bounce off a non-specular surface uses two strategies, combined
        // with multiple importance sampling: a shadow ray to a point sampled on
        // a light (next event estimation), and the BSDF-sampled continuation
        // ray, whose contribution is counted if it happens to hit a light.
        // Throughput beta accumulates f * cos / pdf along the path.
        color ray_color(const ray& camera_ray, const hittable& world, const hittable_list& lights) const {
            color radiance(0, 0, 0);
            color beta(1, 1, 1);
            ray r = camera_ray;
            bool specular = true;  // Camera rays and specular bounces see lights unweighted
            double scatter_pdf = 0;  // Density the current ray's direction was sampled with
            point3 prev_point;       // Where the current ray started

            for (int depth = 0; depth < max_depth; depth++) {
                hit_record rec;
                // Start at 0.001 so a bounced ray doesn't re-hit its own surface (shadow acne)
                if (!world.hit(r, interval(0.001, infinity), rec)) {
                    radiance += beta * background_color(r);
                    break;
                }

                // Emission found by BSDF sampling, weighted against light sampling
                color emission = rec.mat->emitted(r, rec);
                if (emission.x() > 0 || emission.y() > 0 || emission.z() > 0) {
                    if (specular || lights.empty()) {
                        radiance += beta * emission;
                    } else {
                        auto light_pdf = lights.pdf_value(prev_point, r.direction());
                        radiance += beta * emission * power_heuristic(scatter_pdf, light_pdf);
                    }
                }

                scatter_record srec;
                if (!rec.mat->scatter(r, rec, srec))
                    break;

                if (srec.skip_pdf) {
                    beta = beta * srec.attenuation;
                    r = srec.skip_pdf_ray;
                    specular = true;
                } else {
                    if (!lights.empty())
                        radiance += beta * sample_light(r, rec, *srec.pdf_ptr, world, lights);

                    vec3 direction = srec.pdf_ptr->generate();
                    scatter_pdf = srec.pdf_ptr->value(direction);
                    if (scatter_pdf <= 0)
                        break;

                    beta = beta * rec.mat->eval(r, rec, direction) / scatter_pdf;
                    prev_point = rec.p;
                    r = ray(rec.p, direction);
                    specular = false;
                }

                // Russian roulette: after a few bounces, end dim paths at random
                // and boost the survivors so the estimate stays unbiased.
                if (depth >= 3) {
                    auto survive = std::fmin(std::fmax(beta.x(), std::fmax(beta.y(), beta.z())), 0.95);
                    if (random_double() >= survive)
                        break;
                    beta = beta / survive;
                }
            }

            return radiance;
        }

        // Next event estimation: light arriving at rec directly from a point
        // sampled on the lights, times f * cos, MIS weighted against the BSDF
        // sampling density.
        color sample_light(
            const ray& r_in, const hit_record& rec, const pdf& bsdf_pdf,
            const hittable& world, const hittable_list& lights
        ) const {
            vec3 to_light = lights.random(rec.p);
            auto light_pdf = lights.pdf_value(rec.p, to_light);
            if (light_pdf <= 0)
                return color(0, 0, 0);

            color f = rec.mat->eval(r_in, rec, to_light);
            if (f.x() <= 0 && f.y() <= 0 && f.z() <= 0)
                return color(0, 0, 0);

            // The shadow ray sees the light only if nothing else is closer
            ray shadow(rec.p, to_light);
            hit_record light_rec;
            if (!world.hit(shadow, interval(0.001, infinity), light_rec))
                return color(0, 0, 0);

            color emission = light_rec.mat->emitted(shadow, light_rec);
            auto weight = power_heuristic(light_pdf, bsdf_pdf.value(to_light));
            return f * emission * (weight / light_pdf);
        }

        // Radiance arriving from a direction that hits nothing.
        color background_color(const ray& r) const {
            if (!use_sky)
                return background;
            vec3 unit_direction = unit_vector(r.direction());
            auto a = 0.5*(unit_direction.y() + 1.0);
            return (1.0 - a) * color(1.0, 1.0, 1.0) + a*color(0.5, 0.7, 1.0);
        }
};

#endif
