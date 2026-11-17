#include "gart_range_allocator.h"

static bool add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (b > UINT64_MAX - a) return false;
    *out = a + b;
    return true;
}

static bool align_up(uint64_t value, uint64_t alignment, uint64_t *out) {
    uint64_t mask = alignment - 1;
    if (value > UINT64_MAX - mask) return false;
    *out = (value + mask) & ~mask;
    return true;
}

extern "C" bool
gart_range_allocator_init(GARTRangeAllocator *a, GARTFreeRange *storage,
                          size_t capacity, uint64_t total_size) {
    if (!a || !storage || capacity == 0 || total_size == 0) return false;
    a->ranges = storage;
    a->count = 1;
    a->capacity = capacity;
    a->total_size = total_size;
    storage[0].offset = 0;
    storage[0].size = total_size;
    return true;
}

extern "C" bool
gart_range_allocator_alloc(GARTRangeAllocator *a, uint64_t size,
                           uint64_t alignment, uint64_t *out_offset) {
    if (!a || !a->ranges || !out_offset || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0) return false;

    for (size_t i = 0; i < a->count; ++i) {
        const uint64_t start = a->ranges[i].offset;
        const uint64_t length = a->ranges[i].size;
        uint64_t aligned = 0, range_end = 0, alloc_end = 0;
        if (!align_up(start, alignment, &aligned) ||
            !add_u64(start, length, &range_end) ||
            !add_u64(aligned, size, &alloc_end) || alloc_end > range_end)
            continue;

        const bool has_prefix = aligned > start;
        const bool has_suffix = alloc_end < range_end;
        if (has_prefix && has_suffix && a->count == a->capacity) continue;

        if (has_prefix && has_suffix) {
            for (size_t j = a->count; j > i + 1; --j)
                a->ranges[j] = a->ranges[j - 1];
            a->ranges[i].size = aligned - start;
            a->ranges[i + 1].offset = alloc_end;
            a->ranges[i + 1].size = range_end - alloc_end;
            ++a->count;
        } else if (has_prefix) {
            a->ranges[i].size = aligned - start;
        } else if (has_suffix) {
            a->ranges[i].offset = alloc_end;
            a->ranges[i].size = range_end - alloc_end;
        } else {
            for (size_t j = i + 1; j < a->count; ++j)
                a->ranges[j - 1] = a->ranges[j];
            --a->count;
        }
        *out_offset = aligned;
        return true;
    }
    return false;
}

extern "C" bool
gart_range_allocator_free(GARTRangeAllocator *a, uint64_t offset,
                          uint64_t size) {
    if (!a || !a->ranges || size == 0) return false;
    uint64_t end = 0;
    if (!add_u64(offset, size, &end) || end > a->total_size) return false;

    size_t pos = 0;
    while (pos < a->count && a->ranges[pos].offset < offset) ++pos;

    uint64_t prev_end = 0;
    if (pos > 0) {
        if (!add_u64(a->ranges[pos - 1].offset, a->ranges[pos - 1].size,
                     &prev_end) || prev_end > offset) return false;
    }
    if (pos < a->count && end > a->ranges[pos].offset) return false;

    const bool joins_prev = pos > 0 && prev_end == offset;
    const bool joins_next = pos < a->count && end == a->ranges[pos].offset;
    if (joins_prev && joins_next) {
        a->ranges[pos - 1].size += size + a->ranges[pos].size;
        for (size_t j = pos + 1; j < a->count; ++j)
            a->ranges[j - 1] = a->ranges[j];
        --a->count;
    } else if (joins_prev) {
        a->ranges[pos - 1].size += size;
    } else if (joins_next) {
        a->ranges[pos].offset = offset;
        a->ranges[pos].size += size;
    } else {
        if (a->count == a->capacity) return false;
        for (size_t j = a->count; j > pos; --j)
            a->ranges[j] = a->ranges[j - 1];
        a->ranges[pos].offset = offset;
        a->ranges[pos].size = size;
        ++a->count;
    }
    return true;
}
