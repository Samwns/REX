#pragma once
/*
 * rexc_optional.hpp  –  REX Compiler Phase 4 optional type.
 *
 * Header-only rexc_optional<T> with in-place storage,
 * copy/move semantics, and value accessors.
 */

#include <cstddef>
#include <new>

namespace rexc_rt {

// Sentinel type for empty optional construction
struct nullopt_t {
    explicit constexpr nullopt_t(int) {}
};
inline constexpr nullopt_t nullopt{0};

template <typename T>
class rexc_optional {
    alignas(T) unsigned char storage_[sizeof(T)];
    bool has_val_;

    T* ptr()             { return reinterpret_cast<T*>(storage_); }
    const T* ptr() const { return reinterpret_cast<const T*>(storage_); }

    void destroy() {
        if (has_val_) {
            ptr()->~T();
            has_val_ = false;
        }
    }

public:
    // Constructors
    rexc_optional() : has_val_(false) {}

    rexc_optional(nullopt_t) : has_val_(false) {}

    rexc_optional(const T& val) : has_val_(true) {
        new (storage_) T(val);
    }

    rexc_optional(T&& val) : has_val_(true) {
        new (storage_) T(static_cast<T&&>(val));
    }

    // Copy
    rexc_optional(const rexc_optional& o) : has_val_(o.has_val_) {
        if (has_val_) new (storage_) T(*o.ptr());
    }

    rexc_optional& operator=(const rexc_optional& o) {
        if (this != &o) {
            destroy();
            has_val_ = o.has_val_;
            if (has_val_) new (storage_) T(*o.ptr());
        }
        return *this;
    }

    // Move
    rexc_optional(rexc_optional&& o) : has_val_(o.has_val_) {
        if (has_val_) {
            new (storage_) T(static_cast<T&&>(*o.ptr()));
            o.destroy();
        }
    }

    rexc_optional& operator=(rexc_optional&& o) {
        if (this != &o) {
            destroy();
            has_val_ = o.has_val_;
            if (has_val_) {
                new (storage_) T(static_cast<T&&>(*o.ptr()));
                o.destroy();
            }
        }
        return *this;
    }

    // Assign value
    rexc_optional& operator=(const T& val) {
        destroy();
        new (storage_) T(val);
        has_val_ = true;
        return *this;
    }

    rexc_optional& operator=(T&& val) {
        destroy();
        new (storage_) T(static_cast<T&&>(val));
        has_val_ = true;
        return *this;
    }

    ~rexc_optional() { destroy(); }

    // Observers
    bool has_value() const { return has_val_; }
    explicit operator bool() const { return has_val_; }

    T& value() {
        // Undefined behavior if empty (no exceptions in rexc_rt)
        return *ptr();
    }

    const T& value() const {
        return *ptr();
    }

    template <typename U>
    T value_or(U&& default_val) const {
        return has_val_ ? *ptr() : static_cast<T>(static_cast<U&&>(default_val));
    }

    T& operator*()              { return *ptr(); }
    const T& operator*() const  { return *ptr(); }
    T* operator->()             { return ptr(); }
    const T* operator->() const { return ptr(); }

    // Modifiers
    void reset() { destroy(); }
};

} // namespace rexc_rt
