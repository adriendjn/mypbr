#ifndef COLOR_H
#define COLOR_H

#include "interval.h"
#include "vec3.h"

#include <iostream>

using color = vec3;

// Perceived brightness of linear Rec. 709 / sRGB primaries.
inline double luminance(const color& c) {
    return 0.2126 * c.x() + 0.7152 * c.y() + 0.0722 * c.z();
}

// The sRGB transfer curve (IEC 61966-2-1): linear light to encoded value.
inline double linear_to_srgb(double linear_component) {
    if (linear_component <= 0)
        return 0;
    if (linear_component <= 0.0031308)
        return 12.92 * linear_component;
    return 1.055 * std::pow(linear_component, 1.0/2.4) - 0.055;
}

// The inverse curve: sRGB-encoded value in [0, 1] to linear light.
inline double srgb_to_linear(double encoded) {
    if (encoded <= 0.04045)
        return encoded / 12.92;
    return std::pow((encoded + 0.055) / 1.055, 2.4);
}

// Converts a linear [0,1] component to an sRGB-encoded byte.
inline unsigned char to_byte(double linear_component) {
    static const interval intensity(0.000, 0.999);
    return (unsigned char)(256 * intensity.clamp(linear_to_srgb(linear_component)));
}

inline void write_color(std::ostream& out, const color& pixel_color) {
    // Write out the sRGB-encoded pixel color components.
    out << int(to_byte(pixel_color.x())) << ' '
        << int(to_byte(pixel_color.y())) << ' '
        << int(to_byte(pixel_color.z())) << '\n';
}

#endif
