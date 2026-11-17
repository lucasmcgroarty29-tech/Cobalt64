#include "gart_range_allocator.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void basic_reuse_and_coalesce() {
    GARTFreeRange slots[8]{}; GARTRangeAllocator a{}; uint64_t x, y;
    assert(gart_range_allocator_init(&a, slots, 8, 0x10000));
    assert(gart_range_allocator_alloc(&a, 0x2000, 0x1000, &x) && x == 0);
    assert(gart_range_allocator_alloc(&a, 0x3000, 0x4000, &y) && y == 0x4000);
    assert(gart_range_allocator_free(&a, x, 0x2000));
    assert(gart_range_allocator_free(&a, y, 0x3000));
    assert(a.count == 1 && a.ranges[0].offset == 0 && a.ranges[0].size == 0x10000);
}

static void rejects_bad_inputs_and_double_free() {
    GARTFreeRange slots[4]{}; GARTRangeAllocator a{}; uint64_t p = 99;
    assert(!gart_range_allocator_init(&a, slots, 4, 0));
    assert(gart_range_allocator_init(&a, slots, 4, 0x4000));
    assert(!gart_range_allocator_alloc(&a, 0, 4096, &p));
    assert(!gart_range_allocator_alloc(&a, 1, 3, &p));
    assert(!gart_range_allocator_alloc(&a, UINT64_MAX, 4096, &p));
    assert(gart_range_allocator_alloc(&a, 0x1000, 0x1000, &p));
    assert(gart_range_allocator_free(&a, p, 0x1000));
    assert(!gart_range_allocator_free(&a, p, 0x1000));
    assert(!gart_range_allocator_free(&a, UINT64_MAX, 2));
}

static void capacity_failure_is_atomic() {
    GARTFreeRange slots[1]{}; GARTRangeAllocator a{}; uint64_t p;
    assert(gart_range_allocator_init(&a, slots, 1, 0x10000));
    // Consume the first page, leaving a range starting at 0x1000. An aligned
    // allocation at 0x2000 would split that range and needs another descriptor.
    assert(gart_range_allocator_alloc(&a, 0x1000, 0x1000, &p) && p == 0);
    assert(!gart_range_allocator_alloc(&a, 0x1000, 0x2000, &p));
    assert(a.count == 1 && a.ranges[0].offset == 0x1000 && a.ranges[0].size == 0xF000);
}

int main() {
    basic_reuse_and_coalesce();
    rejects_bad_inputs_and_double_free();
    capacity_failure_is_atomic();
    puts("GART range allocator: 3 test groups passed");
    return 0;
}
