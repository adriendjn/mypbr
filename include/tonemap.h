#ifndef TONEMAP_H
#define TONEMAP_H

#include "color3.h"

#include <string>

// Maps scene radiance (unbounded) to display values in [0,1].
enum class tonemapper { none, reinhard, aces };

class display_settings{
    public:
        double     exposure = 0;                // In stops: radiance is scaled by 2^exposure
        tonemapper op       = tonemapper::none; // none clamps, so bright areas clip

        // Parses "none", "reinhard" or "aces"; returns false if unknown.
        bool set_tonemapper(const std::string& name) {
            if (name == "none")          op = tonemapper::none;
            else if (name == "reinhard") op = tonemapper::reinhard;
            else if (name == "aces")     op = tonemapper::aces;
            else return false;
            return true;
        }

        // Exposed, tonemapped linear color, ready for the sRGB curve.
        color apply(const color& radiance) const {
            color c = std::pow(2.0, exposure) * radiance;
            switch (op) {
                case tonemapper::reinhard: return reinhard(c);
                case tonemapper::aces:     return aces_fitted(c);
                case tonemapper::none:     break;
            }
            return c;
        }

    private:
        // Per channel x / (1 + x): never reaches white, compresses highlights.
        static color reinhard(const color& c) {
            return color(c.x() / (1 + c.x()), c.y() / (1 + c.y()), c.z() / (1 + c.z()));
        }

        // Stephen Hill's fit of the ACES reference rendering transform and
        // output device transform, for sRGB/Rec.709 primaries.
        static color aces_fitted(const color& c) {
            // sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
            color v = mul(c, 0.59719, 0.35458, 0.04823,
                             0.07600, 0.90834, 0.01566,
                             0.02840, 0.13383, 0.83777);
            v = color(rrt_odt_fit(v.x()), rrt_odt_fit(v.y()), rrt_odt_fit(v.z()));
            // ODT_SAT => XYZ => D60_2_D65 => sRGB
            v = mul(v,  1.60475, -0.53108, -0.07367,
                       -0.10208,  1.10813, -0.00605,
                       -0.00327, -0.07276,  1.07602);
            return color(clamp01(v.x()), clamp01(v.y()), clamp01(v.z()));
        }

        static double rrt_odt_fit(double x) {
            double a = x * (x + 0.0245786) - 0.000090537;
            double b = x * (0.983729 * x + 0.4329510) + 0.238081;
            return a / b;
        }

        static color mul(const color& c, double m00, double m01, double m02,
                                         double m10, double m11, double m12,
                                         double m20, double m21, double m22) {
            return color(m00*c.x() + m01*c.y() + m02*c.z(),
                         m10*c.x() + m11*c.y() + m12*c.z(),
                         m20*c.x() + m21*c.y() + m22*c.z());
        }

        static double clamp01(double x) {
            return x < 0 ? 0 : (x > 1 ? 1 : x);
        }
};

#endif
