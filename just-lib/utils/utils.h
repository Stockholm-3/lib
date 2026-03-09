#ifndef UTILS_H
#define UTILS_H

#define POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <time.h>

static inline uint64_t system_monotonic_ms() {
    long   ms;
    time_t s;

    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec);
    s  = spec.tv_sec;
    ms = (spec.tv_nsec / 1000000);

    uint64_t result = s;
    result *= 1000;
    result += ms;

    return result;
}

#endif // UTILS_H
