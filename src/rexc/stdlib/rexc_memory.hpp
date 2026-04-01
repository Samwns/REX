#pragma once
/*
 * rexc_memory.hpp  –  REX Compiler Phase 4 smart pointers.
 *
 * Header-only rexc_unique_ptr and rexc_shared_ptr using
 * rexc_malloc / rexc_free from the runtime allocator.
 */

#include <cstddef>
#include <new>
#include "../runtime/rexc_alloc.hpp"

namespace rexc_rt {

// ---------------------------------------------------------------------------
// Default deleter — calls destructor then rexc_free
// ---------------------------------------------------------------------------

template <typename T>
struct default_delete {
    void operator()(T* ptr) const {
        if (ptr) {
            ptr->~T();
            rexc_free(ptr);
        }
    }
};

// ---------------------------------------------------------------------------
// rexc_unique_ptr<T> — move-only RAII wrapper with custom deleter
// ---------------------------------------------------------------------------

template <typename T, typename Deleter = default_delete<T>>
class rexc_unique_ptr {
    T*      ptr_;
    Deleter del_;

public:
    rexc_unique_ptr() : ptr_(nullptr), del_() {}
    explicit rexc_unique_ptr(T* p) : ptr_(p), del_() {}
    rexc_unique_ptr(T* p, Deleter d) : ptr_(p), del_(d) {}

    ~rexc_unique_ptr() { reset(); }

    // Move
    rexc_unique_ptr(rexc_unique_ptr&& o) : ptr_(o.ptr_), del_(static_cast<Deleter&&>(o.del_)) {
        o.ptr_ = nullptr;
    }
    rexc_unique_ptr& operator=(rexc_unique_ptr&& o) {
        if (this != &o) {
            reset();
            ptr_ = o.ptr_;
            del_ = static_cast<Deleter&&>(o.del_);
            o.ptr_ = nullptr;
        }
        return *this;
    }

    // No copy
    rexc_unique_ptr(const rexc_unique_ptr&) = delete;
    rexc_unique_ptr& operator=(const rexc_unique_ptr&) = delete;

    T* get() const          { return ptr_; }
    T& operator*() const    { return *ptr_; }
    T* operator->() const   { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    T* release() {
        T* p = ptr_;
        ptr_ = nullptr;
        return p;
    }

    void reset(T* p = nullptr) {
        if (ptr_) del_(ptr_);
        ptr_ = p;
    }

    Deleter& get_deleter()             { return del_; }
    const Deleter& get_deleter() const { return del_; }
};

// ---------------------------------------------------------------------------
// rexc_shared_ptr<T> — reference-counted (simple, non-atomic for v0.8)
// ---------------------------------------------------------------------------

namespace detail {

struct ref_count_block {
    size_t count;
};

} // namespace detail

template <typename T>
class rexc_shared_ptr {
    T*                       ptr_;
    detail::ref_count_block* cb_;

    void release() {
        if (cb_) {
            --(cb_->count);
            if (cb_->count == 0) {
                if (ptr_) {
                    ptr_->~T();
                    rexc_free(ptr_);
                }
                rexc_free(cb_);
            }
            ptr_ = nullptr;
            cb_  = nullptr;
        }
    }

public:
    rexc_shared_ptr() : ptr_(nullptr), cb_(nullptr) {}

    explicit rexc_shared_ptr(T* p) : ptr_(p), cb_(nullptr) {
        if (p) {
            void* mem = rexc_malloc(sizeof(detail::ref_count_block));
            cb_ = new (mem) detail::ref_count_block{1};
        }
    }

    ~rexc_shared_ptr() { release(); }

    // Copy
    rexc_shared_ptr(const rexc_shared_ptr& o) : ptr_(o.ptr_), cb_(o.cb_) {
        if (cb_) ++(cb_->count);
    }
    rexc_shared_ptr& operator=(const rexc_shared_ptr& o) {
        if (this != &o) {
            release();
            ptr_ = o.ptr_;
            cb_  = o.cb_;
            if (cb_) ++(cb_->count);
        }
        return *this;
    }

    // Move
    rexc_shared_ptr(rexc_shared_ptr&& o) : ptr_(o.ptr_), cb_(o.cb_) {
        o.ptr_ = nullptr;
        o.cb_  = nullptr;
    }
    rexc_shared_ptr& operator=(rexc_shared_ptr&& o) {
        if (this != &o) {
            release();
            ptr_ = o.ptr_;
            cb_  = o.cb_;
            o.ptr_ = nullptr;
            o.cb_  = nullptr;
        }
        return *this;
    }

    T* get() const          { return ptr_; }
    T& operator*() const    { return *ptr_; }
    T* operator->() const   { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    size_t use_count() const { return cb_ ? cb_->count : 0; }

    void reset() { release(); }
    void reset(T* p) {
        release();
        ptr_ = p;
        if (p) {
            void* mem = rexc_malloc(sizeof(detail::ref_count_block));
            cb_ = new (mem) detail::ref_count_block{1};
        }
    }
};

// ---------------------------------------------------------------------------
// Factory helpers
// ---------------------------------------------------------------------------

template <typename T, typename... Args>
inline rexc_unique_ptr<T> make_unique(Args&&... args) {
    void* mem = rexc_malloc(sizeof(T));
    T* obj = new (mem) T(static_cast<Args&&>(args)...);
    return rexc_unique_ptr<T>(obj);
}

template <typename T, typename... Args>
inline rexc_shared_ptr<T> make_shared(Args&&... args) {
    void* mem = rexc_malloc(sizeof(T));
    T* obj = new (mem) T(static_cast<Args&&>(args)...);
    return rexc_shared_ptr<T>(obj);
}

} // namespace rexc_rt
