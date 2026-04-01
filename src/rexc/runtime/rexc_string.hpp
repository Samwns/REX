// rexc_string.hpp — REX Compiler Phase 3 (v0.7) standalone string.
//
// Header-only std::string replacement with Small String Optimization (SSO).
// Strings ≤ 15 bytes are stored inline in a 16-byte buffer; larger strings
// are dynamically allocated via rexc_malloc / rexc_free.
//
// NOTE: Compiled by the host toolchain during the REX build.

#pragma once

#include "rexc_alloc.hpp"
#include <cstddef>
#include <cstdint>

namespace rexc_rt {

// ---------------------------------------------------------------------------
// Helpers (no libc dependency at runtime)
// ---------------------------------------------------------------------------

namespace detail {

inline size_t str_len(const char* s) {
    size_t n = 0;
    if (s) while (s[n]) ++n;
    return n;
}

inline void mem_copy(char* dst, const char* src, size_t n) {
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
}

inline void mem_set(char* dst, char v, size_t n) {
    for (size_t i = 0; i < n; ++i) dst[i] = v;
}

inline int mem_cmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return  1;
    }
    return 0;
}

} // namespace detail

// ---------------------------------------------------------------------------
// rexc_string
// ---------------------------------------------------------------------------

class rexc_string {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);
    static constexpr size_t SSO_CAP = 15; // 16-byte buffer, last byte = '\0'

    // --- Constructors -------------------------------------------------------

    rexc_string() { init_sso(); }

    rexc_string(const char* s) {
        size_t n = detail::str_len(s);
        init_from(s, n);
    }

    rexc_string(const char* s, size_t n) { init_from(s, n); }

    rexc_string(size_t count, char c) {
        init_sso();
        reserve(count);
        for (size_t i = 0; i < count; ++i) push_back(c);
    }

    rexc_string(const rexc_string& o) { init_from(o.c_str(), o.size()); }

    rexc_string(rexc_string&& o) noexcept {
        if (o.is_sso()) {
            detail::mem_copy(sso_buf_, o.sso_buf_, sizeof(sso_buf_));
            size_ = o.size_;
            cap_  = 0;
            heap_ = nullptr;
        } else {
            heap_ = o.heap_;
            size_ = o.size_;
            cap_  = o.cap_;
            sso_buf_[0] = '\0';
        }
        o.init_sso();
    }

    ~rexc_string() { free_heap(); }

    // --- Assignment ---------------------------------------------------------

    rexc_string& operator=(const rexc_string& o) {
        if (this != &o) {
            free_heap();
            init_from(o.c_str(), o.size());
        }
        return *this;
    }

    rexc_string& operator=(rexc_string&& o) noexcept {
        if (this != &o) {
            free_heap();
            if (o.is_sso()) {
                detail::mem_copy(sso_buf_, o.sso_buf_, sizeof(sso_buf_));
                size_ = o.size_;
                cap_  = 0;
                heap_ = nullptr;
            } else {
                heap_ = o.heap_;
                size_ = o.size_;
                cap_  = o.cap_;
                sso_buf_[0] = '\0';
            }
            o.init_sso();
        }
        return *this;
    }

    rexc_string& operator=(const char* s) {
        free_heap();
        init_from(s, detail::str_len(s));
        return *this;
    }

    // --- Capacity -----------------------------------------------------------

    size_t size()   const { return size_; }
    size_t length() const { return size_; }
    bool   empty()  const { return size_ == 0; }

    void reserve(size_t new_cap) {
        if (new_cap <= capacity()) return;
        grow_to(new_cap);
    }

    size_t capacity() const { return is_sso() ? SSO_CAP : cap_; }

    // --- Element access -----------------------------------------------------

    char& operator[](size_t i)       { return data_ptr()[i]; }
    char  operator[](size_t i) const { return c_str()[i]; }

    const char* c_str() const { return is_sso() ? sso_buf_ : heap_; }
    char*       data()        { return data_ptr(); }
    const char* data() const  { return c_str(); }

    // --- Iterators ----------------------------------------------------------

    char*       begin()       { return data_ptr(); }
    char*       end()         { return data_ptr() + size_; }
    const char* begin() const { return c_str(); }
    const char* end()   const { return c_str() + size_; }

    // --- Modifiers ----------------------------------------------------------

    void clear() {
        size_ = 0;
        data_ptr()[0] = '\0';
    }

    void push_back(char c) {
        if (size_ >= capacity()) grow_to(capacity() * 2 + 1);
        data_ptr()[size_++] = c;
        data_ptr()[size_] = '\0';
    }

    rexc_string& append(const char* s, size_t n) {
        if (size_ + n > capacity()) grow_to((size_ + n) * 2);
        detail::mem_copy(data_ptr() + size_, s, n);
        size_ += n;
        data_ptr()[size_] = '\0';
        return *this;
    }

    rexc_string& append(const rexc_string& o) { return append(o.c_str(), o.size()); }
    rexc_string& append(const char* s)         { return append(s, detail::str_len(s)); }

    rexc_string& operator+=(const rexc_string& o) { return append(o); }
    rexc_string& operator+=(const char* s)         { return append(s); }
    rexc_string& operator+=(char c)                { push_back(c); return *this; }

    // --- Search -------------------------------------------------------------

    size_t find(char c, size_t pos = 0) const {
        const char* s = c_str();
        for (size_t i = pos; i < size_; ++i)
            if (s[i] == c) return i;
        return npos;
    }

    size_t find(const char* needle, size_t pos = 0) const {
        size_t nlen = detail::str_len(needle);
        if (nlen == 0) return pos <= size_ ? pos : npos;
        if (nlen > size_) return npos;
        const char* s = c_str();
        for (size_t i = pos; i + nlen <= size_; ++i) {
            if (detail::mem_cmp(s + i, needle, nlen) == 0) return i;
        }
        return npos;
    }

    size_t find(const rexc_string& o, size_t pos = 0) const {
        return find(o.c_str(), pos);
    }

    // --- Substr -------------------------------------------------------------

    rexc_string substr(size_t pos = 0, size_t count = npos) const {
        if (pos >= size_) return rexc_string();
        if (count == npos || pos + count > size_) count = size_ - pos;
        return rexc_string(c_str() + pos, count);
    }

    // --- Comparisons --------------------------------------------------------

    friend bool operator==(const rexc_string& a, const rexc_string& b) {
        if (a.size_ != b.size_) return false;
        return detail::mem_cmp(a.c_str(), b.c_str(), a.size_) == 0;
    }
    friend bool operator!=(const rexc_string& a, const rexc_string& b) { return !(a == b); }
    friend bool operator<(const rexc_string& a, const rexc_string& b) {
        size_t n = a.size_ < b.size_ ? a.size_ : b.size_;
        int r = detail::mem_cmp(a.c_str(), b.c_str(), n);
        if (r != 0) return r < 0;
        return a.size_ < b.size_;
    }

    // --- Concatenation ------------------------------------------------------

    friend rexc_string operator+(const rexc_string& a, const rexc_string& b) {
        rexc_string r;
        r.reserve(a.size_ + b.size_);
        r.append(a);
        r.append(b);
        return r;
    }
    friend rexc_string operator+(const rexc_string& a, const char* b) {
        rexc_string r(a);
        r.append(b);
        return r;
    }
    friend rexc_string operator+(const char* a, const rexc_string& b) {
        rexc_string r(a);
        r.append(b);
        return r;
    }

    // --- Conversions --------------------------------------------------------

    static rexc_string from_int(int64_t v) {
        char buf[21];
        int pos = 0;
        bool neg = v < 0;
        if (neg) v = -v;
        uint64_t uv = static_cast<uint64_t>(v);
        if (uv == 0) buf[pos++] = '0';
        else while (uv > 0) { buf[pos++] = static_cast<char>('0' + (uv % 10)); uv /= 10; }
        if (neg) buf[pos++] = '-';
        // reverse
        for (int i = 0; i < pos / 2; ++i) {
            char t = buf[i]; buf[i] = buf[pos - 1 - i]; buf[pos - 1 - i] = t;
        }
        return rexc_string(buf, static_cast<size_t>(pos));
    }

    static rexc_string from_float(double v, int precision = 6) {
        rexc_string result;
        if (v < 0.0) { result.push_back('-'); v = -v; }
        auto integer_part = static_cast<uint64_t>(v);
        double frac = v - static_cast<double>(integer_part);
        result.append(from_int(static_cast<int64_t>(integer_part)));
        result.push_back('.');
        for (int i = 0; i < precision; ++i) {
            frac *= 10.0;
            int digit = static_cast<int>(frac);
            if (digit > 9) digit = 9;
            result.push_back(static_cast<char>('0' + digit));
            frac -= digit;
        }
        return result;
    }

    int64_t to_int() const {
        const char* s = c_str();
        int64_t v = 0;
        bool neg = false;
        size_t i = 0;
        if (size_ > 0 && s[0] == '-') { neg = true; i = 1; }
        for (; i < size_; ++i) {
            if (s[i] < '0' || s[i] > '9') break;
            v = v * 10 + (s[i] - '0');
        }
        return neg ? -v : v;
    }

    double to_float() const {
        const char* s = c_str();
        double v = 0.0;
        bool neg = false;
        size_t i = 0;
        if (size_ > 0 && s[0] == '-') { neg = true; i = 1; }
        for (; i < size_; ++i) {
            if (s[i] < '0' || s[i] > '9') break;
            v = v * 10.0 + (s[i] - '0');
        }
        if (i < size_ && s[i] == '.') {
            ++i;
            double place = 0.1;
            for (; i < size_; ++i) {
                if (s[i] < '0' || s[i] > '9') break;
                v += (s[i] - '0') * place;
                place *= 0.1;
            }
        }
        return neg ? -v : v;
    }

private:
    char   sso_buf_[16]{};
    char*  heap_ = nullptr;
    size_t size_  = 0;
    size_t cap_   = 0;

    bool is_sso() const { return heap_ == nullptr; }

    char* data_ptr() { return is_sso() ? sso_buf_ : heap_; }

    void init_sso() {
        detail::mem_set(sso_buf_, 0, sizeof(sso_buf_));
        heap_ = nullptr;
        size_ = 0;
        cap_  = 0;
    }

    void free_heap() {
        if (heap_) { rexc_free(heap_); heap_ = nullptr; }
        cap_ = 0;
    }

    void init_from(const char* s, size_t n) {
        size_ = n;
        if (n <= SSO_CAP) {
            heap_ = nullptr;
            cap_  = 0;
            detail::mem_copy(sso_buf_, s, n);
            sso_buf_[n] = '\0';
        } else {
            cap_  = n;
            heap_ = static_cast<char*>(rexc_malloc(n + 1));
            detail::mem_copy(heap_, s, n);
            heap_[n] = '\0';
        }
    }

    void grow_to(size_t new_cap) {
        if (new_cap <= SSO_CAP && is_sso()) return;
        char* new_buf = static_cast<char*>(rexc_malloc(new_cap + 1));
        detail::mem_copy(new_buf, c_str(), size_);
        new_buf[size_] = '\0';
        free_heap();
        heap_ = new_buf;
        cap_  = new_cap;
    }
};

} // namespace rexc_rt
