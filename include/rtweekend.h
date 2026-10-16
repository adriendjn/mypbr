#ifndef RTWEEKEND_H
#define RTWEEKEND_H

#include <cmath>
#include <limits>
#include <random>

// Constants

const double infinity = std::numeric_limits<double>::infinity();
const double pi = 3.1415926535897932385;

// Utility functions

inline double degrees_to_radians(double degrees) {
    return degrees * pi / 180.0;
}

// Returns a random real in [0,1).
// Each thread owns its own generator, so this is safe to call from workers.
inline double random_double() {
    thread_local std::mt19937 generator(std::random_device{}());
    thread_local std::uniform_real_distribution<double> distribution(0.0, 1.0);
    return distribution(generator);
}

// Returns a random real in [min,max).
inline double random_double(double min, double max) {
    return min + (max - min) * random_double();
}

#endif
