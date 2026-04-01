#pragma once
/*
 * rexc_functional.hpp  –  REX Compiler Phase 4 type-erased callable.
 *
 * Header-only rexc_function<R(Args...)> that stores callables via an
 * internal small-buffer or heap allocation using rexc_malloc/rexc_free.
 */

#include <cstddef>
#include <new>
#include "../runtime/rexc_alloc.hpp"

namespace rexc_rt {

// Minimal type traits (avoids <type_traits> dependency)
namespace detail {
    template <typename T> struct remove_ref       { using type = T; };
    template <typename T> struct remove_ref<T&>   { using type = T; };
    template <typename T> struct remove_ref<T&&>  { using type = T; };

    template <typename T> struct remove_cv                { using type = T; };
    template <typename T> struct remove_cv<const T>       { using type = T; };
    template <typename T> struct remove_cv<volatile T>    { using type = T; };
    template <typename T> struct remove_cv<const volatile T> { using type = T; };

    template <typename T> struct decay {
        using type = typename remove_cv<typename remove_ref<T>::type>::type;
    };

    template <typename T, typename U> struct is_same       { static constexpr bool value = false; };
    template <typename T>             struct is_same<T, T> { static constexpr bool value = true;  };

    template <bool B, typename T = void> struct enable_if {};
    template <typename T>                struct enable_if<true, T> { using type = T; };
} // namespace detail

// Primary template (undefined)
template <typename Signature>
class rexc_function;

// ---------------------------------------------------------------------------
// rexc_function<R(Args...)>
// ---------------------------------------------------------------------------

template <typename R, typename... Args>
class rexc_function<R(Args...)> {

    // Internal interface for type erasure
    struct callable_base {
        virtual R invoke(Args... args) = 0;
        virtual callable_base* clone_into(void* dst) const = 0;
        virtual void destroy() = 0;
        virtual size_t size() const = 0;
        virtual ~callable_base() = default;
    };

    template <typename F>
    struct callable_impl : callable_base {
        F func;

        explicit callable_impl(const F& f) : func(f) {}
        explicit callable_impl(F&& f) : func(static_cast<F&&>(f)) {}

        R invoke(Args... args) override {
            return func(static_cast<Args&&>(args)...);
        }

        callable_base* clone_into(void* dst) const override {
            return new (dst) callable_impl<F>(func);
        }

        void destroy() override {
            this->~callable_impl();
        }

        size_t size() const override {
            return sizeof(callable_impl<F>);
        }
    };

    // Small-buffer optimization
    static constexpr size_t BUF_SIZE = 48;
    alignas(alignof(std::max_align_t)) char buf_[BUF_SIZE];

    callable_base* target_;
    bool on_heap_;

    bool fits_in_buffer(size_t sz) const {
        return sz <= BUF_SIZE;
    }

    void clear() {
        if (target_) {
            target_->destroy();
            if (on_heap_) {
                rexc_free(target_);
            }
            target_ = nullptr;
            on_heap_ = false;
        }
    }

public:
    rexc_function() : target_(nullptr), on_heap_(false) {}

    rexc_function(decltype(nullptr)) : target_(nullptr), on_heap_(false) {}

    template <typename F,
              typename = typename detail::enable_if<
                  !detail::is_same<typename detail::decay<F>::type, rexc_function>::value
              >::type>
    rexc_function(F&& f) : target_(nullptr), on_heap_(false) {
        using Impl = callable_impl<typename detail::remove_ref<F>::type>;
        if (sizeof(Impl) <= BUF_SIZE) {
            target_ = new (buf_) Impl(static_cast<F&&>(f));
            on_heap_ = false;
        } else {
            void* mem = rexc_malloc(sizeof(Impl));
            target_ = new (mem) Impl(static_cast<F&&>(f));
            on_heap_ = true;
        }
    }

    ~rexc_function() { clear(); }

    // Copy
    rexc_function(const rexc_function& o) : target_(nullptr), on_heap_(false) {
        if (o.target_) {
            size_t sz = o.target_->size();
            if (sz <= BUF_SIZE) {
                target_ = o.target_->clone_into(buf_);
                on_heap_ = false;
            } else {
                void* mem = rexc_malloc(sz);
                target_ = o.target_->clone_into(mem);
                on_heap_ = true;
            }
        }
    }

    rexc_function& operator=(const rexc_function& o) {
        if (this != &o) {
            clear();
            if (o.target_) {
                size_t sz = o.target_->size();
                if (sz <= BUF_SIZE) {
                    target_ = o.target_->clone_into(buf_);
                    on_heap_ = false;
                } else {
                    void* mem = rexc_malloc(sz);
                    target_ = o.target_->clone_into(mem);
                    on_heap_ = true;
                }
            }
        }
        return *this;
    }

    // Move
    rexc_function(rexc_function&& o) : target_(nullptr), on_heap_(false) {
        if (o.target_) {
            if (o.on_heap_) {
                target_ = o.target_;
                on_heap_ = true;
            } else {
                target_ = o.target_->clone_into(buf_);
                o.target_->destroy();
                on_heap_ = false;
            }
            o.target_ = nullptr;
            o.on_heap_ = false;
        }
    }

    rexc_function& operator=(rexc_function&& o) {
        if (this != &o) {
            clear();
            if (o.target_) {
                if (o.on_heap_) {
                    target_ = o.target_;
                    on_heap_ = true;
                } else {
                    target_ = o.target_->clone_into(buf_);
                    o.target_->destroy();
                    on_heap_ = false;
                }
                o.target_ = nullptr;
                o.on_heap_ = false;
            }
        }
        return *this;
    }

    R operator()(Args... args) const {
        return target_->invoke(static_cast<Args&&>(args)...);
    }

    explicit operator bool() const { return target_ != nullptr; }
};

} // namespace rexc_rt
