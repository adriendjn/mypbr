#ifndef LIGHT_LIST_H
#define LIGHT_LIST_H

#include "environment.h"
#include "hittable_list.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

// The lights next event estimation samples from. Unlike a plain
// hittable_list, which picks one object uniformly, it picks light k with a
// probability p_k proportional to its power, so a dim lamp next to a bright
// sky doesn't take half of the shadow rays.
//
// Choosing light k with probability p_k and then a direction with light k's
// own density pdf_k gives the mixture density sum_k p_k pdf_k. pdf_value()
// returns exactly that, from the same p_k as random(), so f * L / pdf stays
// unbiased whatever the weights are, and the MIS weights that ray_color
// computes when BSDF sampling hits a light use the same density NEE used.
//
// Power is a global measure: it ignores distance and occlusion, so a lamp
// that is weak overall but lights most of the visible scene, or a sky whose
// bright sun is blocked, can be badly under-sampled, and the cost of that is
// unbounded (variance grows like 1 / p_k) while the gain of power over uniform
// selection is bounded. So a fixed fraction of the choices stays uniform
// (defensive sampling), which bounds p_k below by uniform_fraction / n.
//
// It is a hittable_list so the camera can keep taking one: only pdf_value()
// and random() (both virtual) change.
class light_list : public hittable_list{
    public:
        enum class selection { uniform, power };

        selection strategy = selection::power;
        double uniform_fraction = 0.2;  // Share of power selection that stays uniform

        // Computes the selection probabilities; call once every light is
        // added and before rendering (it is not thread safe). The scene bounds
        // give the environment's power. Until it is called, or if lights are
        // added afterwards, selection is uniform: still unbiased, since
        // random() and pdf_value() always agree.
        void build(const aabb& scene_bounds) {
            auto n = objects.size();
            probabilities.assign(n, n > 0 ? 1.0 / n : 0.0);
            if (n == 0)
                return;

            if (strategy == selection::power) {
                // Half the bounding box diagonal: a sphere that holds the scene
                double radius = 0;
                if (scene_bounds.x.size() > 0 && scene_bounds.y.size() > 0 && scene_bounds.z.size() > 0)
                    radius = 0.5 * std::sqrt(scene_bounds.x.size() * scene_bounds.x.size()
                                           + scene_bounds.y.size() * scene_bounds.y.size()
                                           + scene_bounds.z.size() * scene_bounds.z.size());
                if (!std::isfinite(radius) || radius <= 0)
                    radius = 1;

                std::vector<double> power(n);
                double known_sum = 0;
                int known = 0;
                for (size_t k = 0; k < n; k++) {
                    auto sky = std::dynamic_pointer_cast<environment_light>(objects[k]);
                    power[k] = sky ? sky->power(radius) : objects[k]->power();
                    if (std::isfinite(power[k]) && power[k] > 0) {
                        known_sum += power[k];
                        known++;
                    }
                }

                // A light whose power is unknown (or 0) gets the average power
                // of the others rather than 0, which would leave it only the
                // uniform share. Even probability 0 would be unbiased:
                // pdf_value() would be 0 towards it, so emission found by BSDF
                // sampling would get the full MIS weight (power_heuristic(p, 0)
                // = 1). But only BSDF sampling would ever find it, which is very
                // noisy for a small light that does emit.
                double fallback = known > 0 ? known_sum / known : 1.0;
                double total = 0;
                for (auto& p : power) {
                    if (!std::isfinite(p) || p <= 0)
                        p = fallback;
                    total += p;
                }
                for (size_t k = 0; k < n; k++)
                    probabilities[k] = (1 - uniform_fraction) * power[k] / total + uniform_fraction / n;
            }

            cdf.assign(n + 1, 0.0);
            for (size_t k = 0; k < n; k++)
                cdf[k + 1] = cdf[k] + probabilities[k];
            cdf[n] = 1;  // No rounding gap at the top
        }

        // Probability that random() picks light k.
        double probability(size_t k) const {
            if (built())
                return probabilities[k];
            return 1.0 / objects.size();
        }

        // Sum over the lights of p_k * pdf_k, matching random().
        double pdf_value(const point3& origin, const vec3& direction) const override {
            if (objects.empty())
                return 0.0;
            double sum = 0;
            for (size_t k = 0; k < objects.size(); k++)
                sum += probability(k) * objects[k]->pdf_value(origin, direction);
            return sum;
        }

        vec3 random(const point3& origin) const override {
            return objects[pick(random_double())]->random(origin);
        }

        // The light a uniform number xi in [0, 1) selects (inverts the CDF).
        size_t pick(double xi) const {
            auto n = objects.size();
            if (!built())
                return std::min(size_t(xi * n), n - 1);
            auto it = std::upper_bound(cdf.begin(), cdf.end(), xi);
            auto k = size_t(it - cdf.begin());
            return std::clamp(k, size_t(1), n) - 1;
        }

    private:
        std::vector<double> probabilities;
        std::vector<double> cdf;  // n + 1 entries, from 0 to 1

        bool built() const { return cdf.size() == objects.size() + 1; }
};

#endif
