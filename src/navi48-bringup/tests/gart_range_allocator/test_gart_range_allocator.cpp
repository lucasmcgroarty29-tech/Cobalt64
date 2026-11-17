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


static uint32_t next_random(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void randomized_accounting_stress() {
    static constexpr uint64_t page = 0x1000;
    static constexpr uint64_t aperture = 0x100000;
    struct LiveRange { uint64_t offset; uint64_t size; bool live; };
    GARTFreeRange slots[512]{};
    GARTRangeAllocator a{};
    LiveRange live[256]{};
    uint32_t rng = 0x91e10da5u;
    assert(gart_range_allocator_init(&a, slots, 512, aperture));

    for (unsigned step = 0; step < 50000; ++step) {
        const unsigned index = next_random(&rng) % 256u;
        if (live[index].live) {
            assert(gart_range_allocator_free(&a, live[index].offset, live[index].size));
            live[index].live = false;
        } else {
            const uint64_t pages = 1u + (next_random(&rng) % 16u);
            const uint64_t align_pages = 1ull << (next_random(&rng) % 5u);
            uint64_t offset = 0;
            if (gart_range_allocator_alloc(&a, pages * page,
                                           align_pages * page, &offset)) {
                live[index] = {offset, pages * page, true};
            }
        }

        uint64_t accounted = 0;
        uint64_t previous_end = 0;
        for (size_t i = 0; i < a.count; ++i) {
            const uint64_t end = a.ranges[i].offset + a.ranges[i].size;
            assert(a.ranges[i].size != 0);
            assert(end <= aperture);
            assert(i == 0 || a.ranges[i].offset > previous_end);
            accounted += a.ranges[i].size;
            previous_end = end;
        }
        for (unsigned i = 0; i < 256; ++i) {
            if (!live[i].live) continue;
            assert(live[i].size != 0);
            assert(live[i].offset + live[i].size <= aperture);
            accounted += live[i].size;
            for (size_t j = 0; j < a.count; ++j) {
                const uint64_t free_end = a.ranges[j].offset + a.ranges[j].size;
                const uint64_t live_end = live[i].offset + live[i].size;
                assert(live_end <= a.ranges[j].offset ||
                       live[i].offset >= free_end);
            }
            for (unsigned j = i + 1; j < 256; ++j) {
                if (!live[j].live) continue;
                const uint64_t other_end = live[j].offset + live[j].size;
                const uint64_t live_end = live[i].offset + live[i].size;
                assert(live_end <= live[j].offset ||
                       live[i].offset >= other_end);
            }
        }
        assert(accounted == aperture);
    }
}

int main() {
    basic_reuse_and_coalesce();
    rejects_bad_inputs_and_double_free();
    capacity_failure_is_atomic();
    randomized_accounting_stress();
    puts("GART range allocator: 4 test groups passed");
    return 0;
}
