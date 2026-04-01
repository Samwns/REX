// rexc_alloc.hpp — REX Compiler Phase 3 (v0.7) standalone memory allocator.
//
// Header-only free-list allocator using mmap (Linux/macOS) or VirtualAlloc
// (Windows). Uses best-fit strategy with coalescing of adjacent free blocks.
// Block header layout: [size (8 bytes)][flags (8 bytes)][data...]
//   flags: bit 0 = free, bits 1-63 reserved.
//
// NOTE: This header is compiled by the host toolchain during the REX build.
// The generated native binaries will emit raw syscalls instead.

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
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
// Constants
// ---------------------------------------------------------------------------

static constexpr size_t REXC_HEAP_INITIAL_SIZE = 4u * 1024u * 1024u; // 4 MB
static constexpr size_t REXC_BLOCK_MIN_DATA    = 16u;                // alignment
static constexpr uint64_t REXC_FLAG_FREE       = 1u;                 // bit 0

// ---------------------------------------------------------------------------
// Block header
// ---------------------------------------------------------------------------

struct BlockHeader {
    uint64_t size;   // payload size (does NOT include header)
    uint64_t flags;  // bit 0 = free

    bool is_free() const              { return (flags & REXC_FLAG_FREE) != 0; }
    void set_free()                   { flags |= REXC_FLAG_FREE; }
    void set_used()                   { flags &= ~REXC_FLAG_FREE; }
    char* data()                      { return reinterpret_cast<char*>(this) + sizeof(BlockHeader); }
    const char* data() const          { return reinterpret_cast<const char*>(this) + sizeof(BlockHeader); }
    BlockHeader* next()               { return reinterpret_cast<BlockHeader*>(data() + size); }
    const BlockHeader* next() const   { return reinterpret_cast<const BlockHeader*>(data() + size); }
};

static_assert(sizeof(BlockHeader) == 16, "BlockHeader must be 16 bytes");

// ---------------------------------------------------------------------------
// Heap state (process-wide singleton)
// ---------------------------------------------------------------------------

struct HeapRegion {
    char*       base;
    size_t      total;
    HeapRegion* next_region;
};

inline HeapRegion* g_heap_first  = nullptr;
inline bool        g_heap_inited = false;

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------

inline void* platform_alloc(size_t size) {
#if defined(__linux__) || defined(__APPLE__)
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#elif defined(_WIN32)
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#endif
}

inline void platform_free(void* ptr, size_t size) {
#if defined(__linux__) || defined(__APPLE__)
    munmap(ptr, size);
#elif defined(_WIN32)
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#endif
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

inline size_t align_up(size_t v, size_t align) {
    return (v + align - 1) & ~(align - 1);
}

inline HeapRegion* create_region(size_t min_payload) {
    size_t region_meta  = sizeof(HeapRegion);
    size_t block_header = sizeof(BlockHeader);
    size_t needed       = region_meta + block_header + min_payload;
    size_t alloc_size   = needed < REXC_HEAP_INITIAL_SIZE ? REXC_HEAP_INITIAL_SIZE : align_up(needed, 4096);

    char* raw = static_cast<char*>(platform_alloc(alloc_size));
    if (!raw) return nullptr;

    HeapRegion* region  = reinterpret_cast<HeapRegion*>(raw);
    region->base        = raw;
    region->total       = alloc_size;
    region->next_region = nullptr;

    // First block occupies everything after the HeapRegion struct.
    BlockHeader* first = reinterpret_cast<BlockHeader*>(raw + region_meta);
    first->size  = alloc_size - region_meta - block_header;
    first->flags = REXC_FLAG_FREE;

    return region;
}

inline BlockHeader* region_first_block(HeapRegion* r) {
    return reinterpret_cast<BlockHeader*>(r->base + sizeof(HeapRegion));
}

inline bool block_in_region(HeapRegion* r, BlockHeader* b) {
    char* p = reinterpret_cast<char*>(b);
    char* end = r->base + r->total;
    return p >= r->base && p < end;
}

// Try to coalesce `b` with its immediate successor inside `r`.
inline void try_coalesce(HeapRegion* r, BlockHeader* b) {
    if (!b->is_free()) return;
    BlockHeader* n = b->next();
    if (!block_in_region(r, n)) return;
    if (!n->is_free()) return;
    b->size += sizeof(BlockHeader) + n->size;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

inline void rexc_heap_init() {
    if (g_heap_inited) return;
    g_heap_first  = create_region(REXC_HEAP_INITIAL_SIZE);
    g_heap_inited = true;
}

inline void* rexc_malloc(size_t size) {
    if (size == 0) return nullptr;
    if (!g_heap_inited) rexc_heap_init();

    size = align_up(size, REXC_BLOCK_MIN_DATA);
    if (size < REXC_BLOCK_MIN_DATA) size = REXC_BLOCK_MIN_DATA;

    // Best-fit search across all regions.
    BlockHeader* best        = nullptr;
    [[maybe_unused]] HeapRegion* best_region = nullptr;

    for (HeapRegion* r = g_heap_first; r; r = r->next_region) {
        BlockHeader* cur = region_first_block(r);
        char* end = r->base + r->total;
        while (reinterpret_cast<char*>(cur) + sizeof(BlockHeader) <= end) {
            if (cur->is_free() && cur->size >= size) {
                if (!best || cur->size < best->size) {
                    best = cur;
                    best_region = r;
                    if (best->size == size) goto found;  // exact match
                }
            }
            BlockHeader* nx = cur->next();
            if (reinterpret_cast<char*>(nx) <= reinterpret_cast<char*>(cur)) break;
            if (reinterpret_cast<char*>(nx) + sizeof(BlockHeader) > end) break;
            cur = nx;
        }
    }

    if (!best) {
        // Grow: allocate a new region large enough.
        HeapRegion* nr = create_region(size);
        if (!nr) return nullptr;
        nr->next_region = g_heap_first;
        g_heap_first = nr;
        best = region_first_block(nr);
        best_region = nr;
    }

found:
    // Split if there's room for another block.
    if (best->size >= size + sizeof(BlockHeader) + REXC_BLOCK_MIN_DATA) {
        BlockHeader* split = reinterpret_cast<BlockHeader*>(best->data() + size);
        split->size  = best->size - size - sizeof(BlockHeader);
        split->flags = REXC_FLAG_FREE;
        best->size   = size;
    }
    best->set_used();
    return best->data();
}

inline void rexc_free(void* ptr) {
    if (!ptr) return;
    BlockHeader* blk = reinterpret_cast<BlockHeader*>(
        static_cast<char*>(ptr) - sizeof(BlockHeader));
    blk->set_free();

    // Coalesce with successor(s).
    for (HeapRegion* r = g_heap_first; r; r = r->next_region) {
        if (block_in_region(r, blk)) {
            // Forward coalesce.
            try_coalesce(r, blk);
            // Backward coalesce: walk from region start.
            BlockHeader* prev = nullptr;
            BlockHeader* cur  = region_first_block(r);
            char* end = r->base + r->total;
            while (reinterpret_cast<char*>(cur) + sizeof(BlockHeader) <= end && cur != blk) {
                prev = cur;
                BlockHeader* nx = cur->next();
                if (reinterpret_cast<char*>(nx) <= reinterpret_cast<char*>(cur)) break;
                cur = nx;
            }
            if (prev && prev->is_free()) {
                try_coalesce(r, prev);
            }
            break;
        }
    }
}

inline void* rexc_realloc(void* ptr, size_t new_size) {
    if (!ptr) return rexc_malloc(new_size);
    if (new_size == 0) { rexc_free(ptr); return nullptr; }

    BlockHeader* blk = reinterpret_cast<BlockHeader*>(
        static_cast<char*>(ptr) - sizeof(BlockHeader));
    size_t old_size = blk->size;

    new_size = align_up(new_size, REXC_BLOCK_MIN_DATA);
    if (new_size < REXC_BLOCK_MIN_DATA) new_size = REXC_BLOCK_MIN_DATA;

    if (old_size >= new_size) return ptr;  // already big enough

    void* fresh = rexc_malloc(new_size);
    if (!fresh) return nullptr;

    // memcpy without libc
    auto* dst = static_cast<char*>(fresh);
    auto* src = static_cast<char*>(ptr);
    for (size_t i = 0; i < old_size; ++i) dst[i] = src[i];

    rexc_free(ptr);
    return fresh;
}

inline void* rexc_calloc(size_t count, size_t size) {
    size_t total = count * size;
    void* p = rexc_malloc(total);
    if (!p) return nullptr;
    auto* dst = static_cast<char*>(p);
    for (size_t i = 0; i < total; ++i) dst[i] = 0;
    return p;
}

} // namespace rexc_rt
