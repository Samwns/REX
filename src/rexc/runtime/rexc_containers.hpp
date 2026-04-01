// rexc_containers.hpp — REX Compiler Phase 3 (v0.7) standalone containers.
//
// Header-only template containers using rexc_malloc / rexc_free for
// all dynamic allocations.  Provides rexc_vector<T>, rexc_map<K,V>,
// rexc_pair<A,B>, and make_pair helper.
//
// NOTE: Compiled by the host toolchain during the REX build.

#pragma once

#include "rexc_alloc.hpp"
#include <cstddef>
#include <cstdint>
#include <new>       // placement new

namespace rexc_rt {

// ---------------------------------------------------------------------------
// rexc_pair
// ---------------------------------------------------------------------------

template <typename A, typename B>
struct rexc_pair {
    A first;
    B second;

    rexc_pair() : first{}, second{} {}
    rexc_pair(const A& a, const B& b) : first(a), second(b) {}
};

template <typename A, typename B>
inline rexc_pair<A, B> make_pair(const A& a, const B& b) {
    return rexc_pair<A, B>(a, b);
}

// ---------------------------------------------------------------------------
// rexc_vector
// ---------------------------------------------------------------------------

template <typename T>
class rexc_vector {
public:
    rexc_vector() = default;

    rexc_vector(const rexc_vector& o) {
        reserve(o.size_);
        for (size_t i = 0; i < o.size_; ++i) push_back(o.data_[i]);
    }

    rexc_vector(rexc_vector&& o) noexcept
        : data_(o.data_), size_(o.size_), cap_(o.cap_) {
        o.data_ = nullptr;
        o.size_ = 0;
        o.cap_  = 0;
    }

    ~rexc_vector() { destroy_all(); dealloc(); }

    rexc_vector& operator=(const rexc_vector& o) {
        if (this != &o) {
            destroy_all();
            dealloc();
            data_ = nullptr;
            size_ = 0;
            cap_  = 0;
            reserve(o.size_);
            for (size_t i = 0; i < o.size_; ++i) push_back(o.data_[i]);
        }
        return *this;
    }

    rexc_vector& operator=(rexc_vector&& o) noexcept {
        if (this != &o) {
            destroy_all();
            dealloc();
            data_ = o.data_;
            size_ = o.size_;
            cap_  = o.cap_;
            o.data_ = nullptr;
            o.size_ = 0;
            o.cap_  = 0;
        }
        return *this;
    }

    // --- Capacity -----------------------------------------------------------

    size_t size()     const { return size_; }
    bool   empty()    const { return size_ == 0; }
    size_t capacity() const { return cap_; }

    void reserve(size_t new_cap) {
        if (new_cap <= cap_) return;
        T* new_data = static_cast<T*>(rexc_malloc(new_cap * sizeof(T)));
        for (size_t i = 0; i < size_; ++i) {
            ::new (static_cast<void*>(&new_data[i])) T(static_cast<T&&>(data_[i]));
            data_[i].~T();
        }
        dealloc();
        data_ = new_data;
        cap_  = new_cap;
    }

    void resize(size_t new_size) {
        if (new_size > cap_) reserve(new_size);
        for (size_t i = new_size; i < size_; ++i) data_[i].~T();
        for (size_t i = size_; i < new_size; ++i) ::new (static_cast<void*>(&data_[i])) T{};
        size_ = new_size;
    }

    void clear() { destroy_all(); size_ = 0; }

    // --- Element access -----------------------------------------------------

    T& operator[](size_t i)       { return data_[i]; }
    const T& operator[](size_t i) const { return data_[i]; }

    T& at(size_t i)       { return data_[i]; }
    const T& at(size_t i) const { return data_[i]; }

    T& front()             { return data_[0]; }
    const T& front() const { return data_[0]; }
    T& back()              { return data_[size_ - 1]; }
    const T& back()  const { return data_[size_ - 1]; }

    // --- Modifiers ----------------------------------------------------------

    void push_back(const T& v) {
        if (size_ >= cap_) reserve(cap_ == 0 ? 8 : cap_ * 2);
        ::new (static_cast<void*>(&data_[size_])) T(v);
        ++size_;
    }

    void push_back(T&& v) {
        if (size_ >= cap_) reserve(cap_ == 0 ? 8 : cap_ * 2);
        ::new (static_cast<void*>(&data_[size_])) T(static_cast<T&&>(v));
        ++size_;
    }

    void pop_back() {
        if (size_ > 0) { --size_; data_[size_].~T(); }
    }

    // --- Iterators ----------------------------------------------------------

    T*       begin()       { return data_; }
    T*       end()         { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end()   const { return data_ + size_; }

private:
    T*     data_ = nullptr;
    size_t size_ = 0;
    size_t cap_  = 0;

    void destroy_all() {
        for (size_t i = 0; i < size_; ++i) data_[i].~T();
    }
    void dealloc() {
        if (data_) rexc_free(data_);
        data_ = nullptr;
    }
};

// ---------------------------------------------------------------------------
// rexc_map<K,V>  — sorted-array map (simple approach for v0.7)
// ---------------------------------------------------------------------------

template <typename K, typename V>
class rexc_map {
public:
    using value_type = rexc_pair<K, V>;

    rexc_map() = default;

    size_t size()  const { return entries_.size(); }
    bool   empty() const { return entries_.empty(); }

    V& operator[](const K& key) {
        size_t idx = lower_bound(key);
        if (idx < entries_.size() && !(key < entries_[idx].first) && !(entries_[idx].first < key)) {
            return entries_[idx].second;
        }
        // Insert at idx.
        entries_.push_back(value_type{});
        for (size_t i = entries_.size() - 1; i > idx; --i) {
            entries_[i] = static_cast<value_type&&>(entries_[i - 1]);
        }
        entries_[idx].first  = key;
        entries_[idx].second = V{};
        return entries_[idx].second;
    }

    size_t count(const K& key) const {
        size_t idx = lower_bound(key);
        if (idx < entries_.size() && !(key < entries_[idx].first) && !(entries_[idx].first < key))
            return 1;
        return 0;
    }

    void erase(const K& key) {
        size_t idx = lower_bound(key);
        if (idx < entries_.size() && !(key < entries_[idx].first) && !(entries_[idx].first < key)) {
            for (size_t i = idx; i + 1 < entries_.size(); ++i) {
                entries_[i] = static_cast<value_type&&>(entries_[i + 1]);
            }
            entries_.pop_back();
        }
    }

    // --- Iterators (delegate to underlying vector) --------------------------

    value_type*       begin()       { return entries_.begin(); }
    value_type*       end()         { return entries_.end(); }
    const value_type* begin() const { return entries_.begin(); }
    const value_type* end()   const { return entries_.end(); }

private:
    rexc_vector<value_type> entries_;

    size_t lower_bound(const K& key) const {
        size_t lo = 0, hi = entries_.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (entries_[mid].first < key) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }
};

} // namespace rexc_rt
