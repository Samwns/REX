#pragma once
/*
 * rexc_cstdlib.hpp  –  REX Compiler Phase 4 C stdlib functions.
 *
 * Header-only inline implementations of atoi, atof, rand/srand, exit,
 * and abort.  All live in namespace rexc_rt.
 */

#include <cstddef>
#include <cstdint>
#include "../runtime/rexc_startup.hpp"

namespace rexc_rt {

// ---------------------------------------------------------------------------
// atoi / atof
// ---------------------------------------------------------------------------

inline int rexc_atoi(const char* s) {
    if (!s) return 0;
    int result = 0;
    bool neg = false;
    size_t i = 0;

    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n') ++i;
    if (s[i] == '-') { neg = true; ++i; }
    else if (s[i] == '+') { ++i; }

    while (s[i] >= '0' && s[i] <= '9') {
        result = result * 10 + (s[i] - '0');
        ++i;
    }
    return neg ? -result : result;
}

inline double rexc_atof(const char* s) {
    if (!s) return 0.0;
    double result = 0.0;
    bool neg = false;
    size_t i = 0;

    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n') ++i;
    if (s[i] == '-') { neg = true; ++i; }
    else if (s[i] == '+') { ++i; }

    // Integer part
    while (s[i] >= '0' && s[i] <= '9') {
        result = result * 10.0 + (s[i] - '0');
        ++i;
    }

    // Fractional part
    if (s[i] == '.') {
        ++i;
        double place = 0.1;
        while (s[i] >= '0' && s[i] <= '9') {
            result += (s[i] - '0') * place;
            place *= 0.1;
            ++i;
        }
    }

    // Simple exponent support (e.g. 1e3, 2.5e-4)
    if (s[i] == 'e' || s[i] == 'E') {
        ++i;
        bool exp_neg = false;
        if (s[i] == '-') { exp_neg = true; ++i; }
        else if (s[i] == '+') { ++i; }

        int exp_val = 0;
        while (s[i] >= '0' && s[i] <= '9') {
            exp_val = exp_val * 10 + (s[i] - '0');
            ++i;
        }

        double factor = 1.0;
        for (int e = 0; e < exp_val; ++e) factor *= 10.0;
        if (exp_neg) result /= factor;
        else result *= factor;
    }

    return neg ? -result : result;
}

// ---------------------------------------------------------------------------
// rand / srand  –  simple Linear Congruential Generator
// ---------------------------------------------------------------------------

namespace detail {
    inline uint64_t rexc_rand_state = 1;
} // namespace detail

inline void rexc_srand(unsigned int seed) {
    detail::rexc_rand_state = seed;
}

inline int rexc_rand() {
    // LCG parameters from Numerical Recipes
    detail::rexc_rand_state = detail::rexc_rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<int>((detail::rexc_rand_state >> 33) & 0x7FFFFFFF);
}

// ---------------------------------------------------------------------------
// exit / abort  (delegate to the runtime's rexc_exit from rexc_startup.hpp)
// ---------------------------------------------------------------------------

[[noreturn]] inline void rexc_stdlib_exit(int code) {
    rexc_exit(code);
}

[[noreturn]] inline void rexc_stdlib_abort() {
    rexc_exit(134); // 128 + SIGABRT(6) = 134
}

} // namespace rexc_rt
