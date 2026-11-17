// Pure host-testable free-range allocator for GART aperture offsets.
// This module does not write page-table entries or perform GPU TLB invalidation.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GARTFreeRange {
    uint64_t offset;
    uint64_t size;
} GARTFreeRange;

typedef struct GARTRangeAllocator {
    GARTFreeRange *ranges;
    size_t count;
    size_t capacity;
    uint64_t total_size;
    uint64_t granularity;
} GARTRangeAllocator;

// The caller owns the fixed-capacity range storage for the allocator's lifetime.
// On success, the entire [0, total_size) aperture is initially free.
bool gart_range_allocator_init(GARTRangeAllocator *allocator,
                               GARTFreeRange *storage,
                               size_t storage_capacity,
                               uint64_t total_size,
                               uint64_t granularity);

// First-fit allocation. Size must be a multiple of granularity; alignment
// must be a power of two and at least granularity. Failure is state-atomic.
bool gart_range_allocator_alloc(GARTRangeAllocator *allocator,
                                uint64_t size,
                                uint64_t alignment,
                                uint64_t *out_offset);

// Return a previously allocated range. Offset and size must be granularity
// multiples. Rejects out-of-bounds, overlapping, duplicate and malformed frees.
bool gart_range_allocator_free(GARTRangeAllocator *allocator,
                               uint64_t offset,
                               uint64_t size);

#ifdef __cplusplus
}
#endif
