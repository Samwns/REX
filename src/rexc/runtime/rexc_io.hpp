// rexc_io.hpp — REX Compiler Phase 3 (v0.7) standalone I/O primitives.
//
// Header-only I/O using write()/read() syscalls on Linux/macOS and
// WriteFile/ReadFile on Windows.  Provides low-level helpers plus
// OStream / IStream wrappers with operator<< / operator>> and global
// cout, cerr, cin instances.
//
// NOTE: Compiled by the host toolchain during the REX build.
// Generated native binaries will emit raw syscalls directly.

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#error "Unsupported platform"
#endif

namespace rexc_rt {

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------

inline size_t cstr_len(const char* s) {
    size_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

inline void write_str(int fd, const char* s, size_t len) {
    if (!s || len == 0) return;
#if defined(__linux__) || defined(__APPLE__)
    while (len > 0) {
        auto w = ::write(fd, s, len);
        if (w <= 0) break;
        s   += static_cast<size_t>(w);
        len -= static_cast<size_t>(w);
    }
#elif defined(_WIN32)
    HANDLE h = (fd == 2) ? GetStdHandle(STD_ERROR_HANDLE)
             : (fd == 0) ? GetStdHandle(STD_INPUT_HANDLE)
                         : GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    while (len > 0) {
        if (!WriteFile(h, s, static_cast<DWORD>(len), &written, nullptr)) break;
        s   += written;
        len -= written;
    }
#endif
}

inline void write_cstr(int fd, const char* s) {
    write_str(fd, s, cstr_len(s));
}

inline void write_char(int fd, char c) {
    write_str(fd, &c, 1);
}

inline void write_newline(int fd) {
    write_char(fd, '\n');
}

inline void write_uint(int fd, uint64_t v) {
    if (v == 0) { write_char(fd, '0'); return; }
    char buf[20];
    int  pos = 0;
    while (v > 0) { buf[pos++] = static_cast<char>('0' + (v % 10)); v /= 10; }
    // reverse
    for (int i = 0; i < pos / 2; ++i) {
        char t = buf[i]; buf[i] = buf[pos - 1 - i]; buf[pos - 1 - i] = t;
    }
    write_str(fd, buf, static_cast<size_t>(pos));
}

inline void write_int(int fd, int64_t v) {
    if (v < 0) { write_char(fd, '-'); v = -v; }
    write_uint(fd, static_cast<uint64_t>(v));
}

inline void write_float(int fd, double v, int precision = 6) {
    if (v < 0.0) { write_char(fd, '-'); v = -v; }

    auto integer_part = static_cast<uint64_t>(v);
    double frac = v - static_cast<double>(integer_part);

    write_uint(fd, integer_part);
    write_char(fd, '.');

    for (int i = 0; i < precision; ++i) {
        frac *= 10.0;
        int digit = static_cast<int>(frac);
        if (digit > 9) digit = 9;
        write_char(fd, static_cast<char>('0' + digit));
        frac -= digit;
    }
}

inline size_t read_line(char* buf, size_t max_len) {
    if (!buf || max_len == 0) return 0;
    size_t i = 0;
#if defined(__linux__) || defined(__APPLE__)
    while (i < max_len - 1) {
        char c;
        auto r = ::read(0, &c, 1);
        if (r <= 0) break;
        if (c == '\n') break;
        buf[i++] = c;
    }
#elif defined(_WIN32)
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    while (i < max_len - 1) {
        char c;
        DWORD nread = 0;
        if (!ReadFile(h, &c, 1, &nread, nullptr) || nread == 0) break;
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[i++] = c;
    }
#endif
    buf[i] = '\0';
    return i;
}

inline int read_char() {
#if defined(__linux__) || defined(__APPLE__)
    char c;
    auto r = ::read(0, &c, 1);
    return (r <= 0) ? -1 : static_cast<int>(static_cast<unsigned char>(c));
#elif defined(_WIN32)
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    char c;
    DWORD nread = 0;
    if (!ReadFile(h, &c, 1, &nread, nullptr) || nread == 0) return -1;
    return static_cast<int>(static_cast<unsigned char>(c));
#endif
}

// ---------------------------------------------------------------------------
// Manipulator tag
// ---------------------------------------------------------------------------

struct EndlTag {};
inline EndlTag endl;

// ---------------------------------------------------------------------------
// OStream
// ---------------------------------------------------------------------------

struct OStream {
    int fd;

    explicit OStream(int file_desc) : fd(file_desc) {}

    OStream& operator<<(const char* s)  { write_cstr(fd, s);  return *this; }
    OStream& operator<<(char c)         { write_char(fd, c);  return *this; }
    OStream& operator<<(int64_t v)      { write_int(fd, v);   return *this; }
    OStream& operator<<(int v)          { write_int(fd, static_cast<int64_t>(v)); return *this; }
    OStream& operator<<(uint64_t v)     { write_uint(fd, v);  return *this; }
    OStream& operator<<(double v)       { write_float(fd, v); return *this; }
    OStream& operator<<(EndlTag)        { write_newline(fd);  return *this; }
};

// ---------------------------------------------------------------------------
// IStream
// ---------------------------------------------------------------------------

struct IStream {
    int fd;

    explicit IStream(int file_desc) : fd(file_desc) {}

    IStream& operator>>(int64_t& v) {
        char buf[32];
        size_t n = read_line(buf, sizeof(buf));
        v = 0;
        bool neg = false;
        size_t start = 0;
        if (n > 0 && buf[0] == '-') { neg = true; start = 1; }
        for (size_t i = start; i < n; ++i) {
            if (buf[i] < '0' || buf[i] > '9') break;
            v = v * 10 + (buf[i] - '0');
        }
        if (neg) v = -v;
        return *this;
    }

    IStream& operator>>(double& v) {
        char buf[64];
        size_t n = read_line(buf, sizeof(buf));
        v = 0.0;
        bool neg = false;
        size_t i = 0;
        if (n > 0 && buf[0] == '-') { neg = true; i = 1; }
        // integer part
        for (; i < n; ++i) {
            if (buf[i] < '0' || buf[i] > '9') break;
            v = v * 10.0 + (buf[i] - '0');
        }
        // fractional part
        if (i < n && buf[i] == '.') {
            ++i;
            double place = 0.1;
            for (; i < n; ++i) {
                if (buf[i] < '0' || buf[i] > '9') break;
                v += (buf[i] - '0') * place;
                place *= 0.1;
            }
        }
        if (neg) v = -v;
        return *this;
    }

    IStream& operator>>(char* s) {
        if (s) read_line(s, 256);
        return *this;
    }

    IStream& operator>>(char& c) {
        int r = read_char();
        c = (r >= 0) ? static_cast<char>(r) : '\0';
        return *this;
    }
};

// ---------------------------------------------------------------------------
// Global instances
// ---------------------------------------------------------------------------

inline OStream cout{1};
inline OStream cerr{2};
inline IStream cin{0};

} // namespace rexc_rt
