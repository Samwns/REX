#pragma once
/*
 * rexc_algorithm.hpp  –  REX Compiler Phase 4 standard algorithms.
 *
 * Header-only implementations of common algorithms operating on
 * raw pointers / iterators.  All live in namespace rexc_rt.
 */

#include <cstddef>

namespace rexc_rt {

// ---------------------------------------------------------------------------
// Utility: swap, min, max
// ---------------------------------------------------------------------------

template <typename T>
inline void swap(T& a, T& b) {
    T tmp = static_cast<T&&>(a);
    a = static_cast<T&&>(b);
    b = static_cast<T&&>(tmp);
}

template <typename T>
inline const T& min(const T& a, const T& b) {
    return (b < a) ? b : a;
}

template <typename T>
inline const T& max(const T& a, const T& b) {
    return (a < b) ? b : a;
}

// ---------------------------------------------------------------------------
// find, find_if
// ---------------------------------------------------------------------------

template <typename Iter, typename T>
inline Iter find(Iter first, Iter last, const T& value) {
    for (; first != last; ++first) {
        if (*first == value) return first;
    }
    return last;
}

template <typename Iter, typename Pred>
inline Iter find_if(Iter first, Iter last, Pred pred) {
    for (; first != last; ++first) {
        if (pred(*first)) return first;
    }
    return last;
}

// ---------------------------------------------------------------------------
// copy, fill
// ---------------------------------------------------------------------------

template <typename InputIt, typename OutputIt>
inline OutputIt copy(InputIt first, InputIt last, OutputIt d_first) {
    for (; first != last; ++first, ++d_first) {
        *d_first = *first;
    }
    return d_first;
}

template <typename Iter, typename T>
inline void fill(Iter first, Iter last, const T& value) {
    for (; first != last; ++first) {
        *first = value;
    }
}

// ---------------------------------------------------------------------------
// Insertion sort (for small ranges, used by introsort)
// ---------------------------------------------------------------------------

namespace detail {

template <typename Iter>
inline void insertion_sort(Iter first, Iter last) {
    if (first == last) return;
    for (Iter i = first + 1; i != last; ++i) {
        auto key = static_cast<decltype(*i)&&>(*i);
        Iter j = i;
        while (j != first && key < *(j - 1)) {
            *j = static_cast<decltype(*j)&&>(*(j - 1));
            --j;
        }
        *j = static_cast<decltype(*j)&&>(key);
    }
}

// Median-of-three pivot selection
template <typename Iter>
inline Iter median_of_three(Iter a, Iter b, Iter c) {
    if (*a < *b) {
        if (*b < *c) return b;      // a < b < c
        if (*a < *c) return c;      // a < c <= b
        return a;                    // c <= a < b
    }
    // b <= a
    if (*a < *c) return a;          // b <= a < c
    if (*b < *c) return c;          // b < c <= a
    return b;                        // c <= b <= a
}

// Partition around pivot value
template <typename Iter>
inline Iter partition(Iter first, Iter last) {
    Iter mid = first + (last - first) / 2;
    Iter pivot_it = median_of_three(first, mid, last - 1);
    rexc_rt::swap(*pivot_it, *(last - 1));

    auto& pivot = *(last - 1);
    Iter store = first;
    for (Iter it = first; it != last - 1; ++it) {
        if (*it < pivot) {
            rexc_rt::swap(*it, *store);
            ++store;
        }
    }
    rexc_rt::swap(*store, *(last - 1));
    return store;
}

// Floor-log2 for depth limit
inline size_t floor_log2(size_t n) {
    size_t log = 0;
    while (n > 1) { n >>= 1; ++log; }
    return log;
}

// Introsort recursive core
template <typename Iter>
inline void introsort_impl(Iter first, Iter last, size_t depth_limit) {
    while (static_cast<size_t>(last - first) > 16) {
        if (depth_limit == 0) {
            // Fall back to insertion sort for safety at deep recursion
            insertion_sort(first, last);
            return;
        }
        --depth_limit;
        Iter pivot = partition(first, last);
        // Recurse on smaller partition, iterate on larger
        if (pivot - first < last - pivot) {
            introsort_impl(first, pivot, depth_limit);
            first = pivot + 1;
        } else {
            introsort_impl(pivot + 1, last, depth_limit);
            last = pivot;
        }
    }
    insertion_sort(first, last);
}

} // namespace detail

// ---------------------------------------------------------------------------
// sort  –  introsort (quicksort + insertion sort for small ranges)
// ---------------------------------------------------------------------------

template <typename Iter>
inline void sort(Iter first, Iter last) {
    if (first == last) return;
    size_t n = static_cast<size_t>(last - first);
    if (n <= 1) return;
    size_t depth_limit = 2 * detail::floor_log2(n);
    detail::introsort_impl(first, last, depth_limit);
}

} // namespace rexc_rt
