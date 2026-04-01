#pragma once
/*
 * rexc_cstring.hpp  –  REX Compiler Phase 4 C string functions.
 *
 * Header-only inline implementations of common C string and memory
 * operations.  All live in namespace rexc_rt.
 */

#include <cstddef>

namespace rexc_rt {

// ---------------------------------------------------------------------------
// String length
// ---------------------------------------------------------------------------

inline size_t rexc_strlen(const char* s) {
    if (!s) return 0;
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// String copy
// ---------------------------------------------------------------------------

inline char* rexc_strcpy(char* dst, const char* src) {
    if (!dst || !src) return dst;
    char* d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

inline char* rexc_strncpy(char* dst, const char* src, size_t n) {
    if (!dst || !src) return dst;
    size_t i = 0;
    for (; i < n && src[i]; ++i) dst[i] = src[i];
    for (; i < n; ++i) dst[i] = '\0';
    return dst;
}

// ---------------------------------------------------------------------------
// String compare
// ---------------------------------------------------------------------------

inline int rexc_strcmp(const char* a, const char* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && *a == *b) { ++a; ++b; }
    return static_cast<int>(static_cast<unsigned char>(*a)) -
           static_cast<int>(static_cast<unsigned char>(*b));
}

inline int rexc_strncmp(const char* a, const char* b, size_t n) {
    if (n == 0) return 0;
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i] || a[i] == '\0') {
            return static_cast<int>(static_cast<unsigned char>(a[i])) -
                   static_cast<int>(static_cast<unsigned char>(b[i]));
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Memory operations
// ---------------------------------------------------------------------------

inline void* rexc_memcpy(void* dst, const void* src, size_t n) {
    if (!dst || !src) return dst;
    auto* d = static_cast<unsigned char*>(dst);
    const auto* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

inline void* rexc_memset(void* dst, int val, size_t n) {
    if (!dst) return dst;
    auto* d = static_cast<unsigned char*>(dst);
    auto v = static_cast<unsigned char>(val);
    for (size_t i = 0; i < n; ++i) d[i] = v;
    return dst;
}

inline void* rexc_memmove(void* dst, const void* src, size_t n) {
    if (!dst || !src || n == 0) return dst;
    auto* d = static_cast<unsigned char*>(dst);
    const auto* s = static_cast<const unsigned char*>(src);
    if (d < s) {
        for (size_t i = 0; i < n; ++i) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; --i) d[i - 1] = s[i - 1];
    }
    return dst;
}

} // namespace rexc_rt
