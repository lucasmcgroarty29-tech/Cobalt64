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
} GARTRangeAllocator;

// The caller owns the fixed-capacity range storage for the allocator's lifetime.
// On success, the entire [0, total_size) aperture is initially free.
bool gart_range_allocator_init(GARTRangeAllocator *allocator,
                               GARTFreeRange *storage,
                               size_t storage_capacity,
                               uint64_t total_size);

// First-fit allocation with power-of-two alignment. Returns false without
// changing allocator state when the request cannot be satisfied.
bool gart_range_allocator_alloc(GARTRangeAllocator *allocator,
                                uint64_t size,
                                uint64_t alignment,
                                uint64_t *out_offset);

// Return a previously allocated range. Rejects out-of-bounds, overlapping,
// duplicate, and malformed frees. Adjacent ranges are coalesced.
bool gart_range_allocator_free(GARTRangeAllocator *allocator,
                               uint64_t offset,
                               uint64_t size);

#ifdef __cplusplus
}
#endif
