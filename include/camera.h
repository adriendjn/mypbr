#ifndef CAMERA_H
#define CAMERA_H

#include "hittable.h"
#include "color3.h"
#include "hittable_list.h"
#include "material.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <iostream>
#include <thread>
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

        int    threads = 0;  // Worker threads; 0 uses every hardware thread

        int height() const { return image_height; }

        // Called during a progressive render with the number of samples per
        // pixel done so far and the image averaged over them.
        using pass_callback = std::function<void(int, const std::vector<color>&)>;

        // Renders the world and returns the linear radiance of every pixel,
        // row by row from the top left. lights are the emitters to sample
        // directly (next event estimation); they must also be part of world.
        //
        // Rendering is progressive: each pass adds one sample to every pixel.
        // If save_every > 0, on_pass is called every save_every passes so a
        // long render can be stopped early with a usable image.
        std::vector<color> render(const hittable& world, const hittable_list& lights,
                                  int save_every = 0, const pass_callback& on_pass = nullptr) {
            initialize();

            size_t pixel_count = size_t(image_width) * image_height;
            std::vector<color> sum(pixel_count);

            int worker_count = threads > 0 ? threads : int(std::thread::hardware_concurrency());
            worker_count = std::max(worker_count, 1);

            for (int pass = 0; pass < samples_per_pixel; pass++) {
                std::clog << "\rPass " << (pass + 1) << '/' << samples_per_pixel << ' ' << std::flush;
                render_pass(pass, worker_count, world, lights, sum);

                int done = pass + 1;
                if (on_pass && save_every > 0 && done % save_every == 0 && done < samples_per_pixel)
                    on_pass(done, average(sum, done));
            }
            std::clog << "\rDone.                   \n";
            return average(sum, samples_per_pixel);
        }

        // Adds one sample to every pixel of sum. The image is cut into square
        // tiles that worker threads claim from a shared atomic counter; each
        // pixel belongs to one tile, so workers never write the same pixel.
        void render_pass(int pass, int worker_count, const hittable& world, const hittable_list& lights,
                         std::vector<color>& sum) const {
            const int tile_size = 16;
            int tiles_x = (image_width + tile_size - 1) / tile_size;
            int tiles_y = (image_height + tile_size - 1) / tile_size;
            int tile_count = tiles_x * tiles_y;
            std::atomic<int> next_tile{0};

            auto worker = [&]() {
                while (true) {
                    int tile = next_tile.fetch_add(1);
                    if (tile >= tile_count)
                        return;
                    int i0 = (tile % tiles_x) * tile_size;
                    int j0 = (tile / tiles_x) * tile_size;
                    int i1 = std::min(i0 + tile_size, image_width);
                    int j1 = std::min(j0 + tile_size, image_height);
                    for (int j = j0; j < j1; j++)
                        for (int i = i0; i < i1; i++)
                            sum[size_t(j) * image_width + i] += render_sample(i, j, pass, world, lights);
                }
            };

            std::vector<std::thread> pool;
            for (int k = 1; k < std::min(worker_count, tile_count); k++)
                pool.emplace_back(worker);
            worker();  // The calling thread works too
            for (auto& t : pool)
                t.join();
        }

        // Computes the image height and camera basis; render() calls it.
        void initialize() {
            image_height = int(image_width / aspect_ratio);
            image_height = (image_height < 1) ? 1 : image_height;

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
        int    sqrt_spp;             // Square root of number of samples per pixel
        double recip_sqrt_spp;       // 1 / sqrt_spp
        point3 center;         // Camera center
        point3 pixel00_loc;    // Location of pixel 0, 0
        vec3   pixel_delta_u;  // Offset to pixel to the right
        vec3   pixel_delta_v;  // Offset to pixel below
        vec3   u, v, w;        // Camera frame basis vectors
        vec3   defocus_disk_u; // Defocus disk horizontal radius
        vec3   defocus_disk_v; // Defocus disk vertical radius

        // One radiance sample for pixel i, j. Across the passes, each pixel's
        // first sqrt_spp^2 samples visit every cell of a sqrt_spp x sqrt_spp grid
        // once (jittered within the cell); the cell order is offset per pixel
        // so partial images aren't biased toward one corner. Any remaining
        // samples are placed uniformly at random.
        color render_sample(int i, int j, int pass, const hittable& world, const hittable_list& lights) const {
            int strata = sqrt_spp * sqrt_spp;
            if (pass >= strata)
                return sample_radiance(get_ray(i, j, sample_square()), world, lights);

            int cell = (pass + i * 7919 + j * 104729) % strata;
            auto offset = sample_square_stratified(cell % sqrt_spp, cell / sqrt_spp);
            return sample_radiance(get_ray(i, j, offset), world, lights);
        }

        static std::vector<color> average(const std::vector<color>& sum, int samples) {
            std::vector<color> image(sum.size());
            double scale = 1.0 / samples;
            for (size_t k = 0; k < sum.size(); k++)
                image[k] = scale * sum[k];
            return image;
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
                    // On the last bounce no BSDF ray follows to find the light,
                    // so the light sample takes the whole contribution
                    bool last_bounce = depth == max_depth - 1;
                    if (!lights.empty())
                        radiance += beta * sample_light(r, rec, *srec.pdf_ptr, world, lights, last_bounce);

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
        // sampling density (unweighted on the last bounce).
        color sample_light(
            const ray& r_in, const hit_record& rec, const pdf& bsdf_pdf,
            const hittable& world, const hittable_list& lights, bool last_bounce
        ) const {
            vec3 to_light = lights.random(rec.p);
            auto light_pdf = lights.pdf_value(rec.p, to_light);
            if (light_pdf <= 0)
                return color(0, 0, 0);

            color f = rec.mat->eval(r_in, rec, to_light);
            if (f.x() <= 0 && f.y() <= 0 && f.z() <= 0)
                return color(0, 0, 0);

            // The shadow ray sees the light only if nothing else is closer. Its
            // direction is normalized so the 0.001 offset is a distance: to_light
            // reaches the light, and would make it skip a whole unit of the scene.
            ray shadow(rec.p, unit_vector(to_light));
            hit_record light_rec;
            if (!world.hit(shadow, interval(0.001, infinity), light_rec))
                return color(0, 0, 0);

            color emission = light_rec.mat->emitted(shadow, light_rec);
            auto weight = last_bounce ? 1.0 : power_heuristic(light_pdf, bsdf_pdf.value(to_light));
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
