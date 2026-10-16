#ifndef COLOR_H
#define COLOR_H

#include "interval.h"
#include "vec3.h"

#include <iostream>

using color = vec3;

inline double linear_to_gamma(double linear_component) {
    if (linear_component > 0)
        return std::sqrt(linear_component);
    return 0;
}

// Converts a linear [0,1] component to a gamma-encoded byte.
inline unsigned char to_byte(double linear_component) {
    static const interval intensity(0.000, 0.999);
    return (unsigned char)(256 * intensity.clamp(linear_to_gamma(linear_component)));
}

inline void write_color(std::ostream& out, const color& pixel_color) {
    // Write out the gamma-encoded pixel color components.
    out << int(to_byte(pixel_color.x())) << ' '
        << int(to_byte(pixel_color.y())) << ' '
        << int(to_byte(pixel_color.z())) << '\n';
}

#endif