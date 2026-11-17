#include "gart_range_bounds.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <algorithm>
#include <thread>
#include <vector>

static void test_page_spans(void)
{
    assert(amdgpu_range_fits(16, 0, 16));
    assert(amdgpu_range_fits(16, 16, 0));
    assert(!amdgpu_range_fits(16, 17, 0));
    assert(!amdgpu_range_fits(16, UINT64_MAX, 1));
    uint64_t rounded = 0;
    uint32_t pages = 0;
    assert(gart_checked_page_span(1, &rounded, &pages));
    assert(rounded == 4096 && pages == 1);
    assert(gart_checked_page_span(4096, &rounded, &pages));
    assert(rounded == 4096 && pages == 1);
    assert(gart_checked_page_span(4097, &rounded, &pages));
    assert(rounded == 8192 && pages == 2);
    assert(!gart_checked_page_span(0, &rounded, &pages));
    assert(!gart_checked_page_span(UINT64_MAX, &rounded, &pages));
    assert(!gart_checked_page_span(1, nullptr, &pages));
    assert(!gart_checked_page_span(1, &rounded, nullptr));
    assert(!gart_checked_page_span((uint64_t)UINT32_MAX * 4096ULL + 1ULL,
                                   &rounded, &pages));
}

static void test_ranges_and_pte_bounds(void)
{
    assert(gart_checked_range(16384, 0, 4096));
    assert(gart_checked_range(16384, 12288, 4096));
    assert(!gart_checked_range(16384, 16384, 4096));
    assert(!gart_checked_range(16384, UINT64_MAX, 4096));
    assert(!gart_checked_range(16384, 0, 0));
    assert(gart_ranges_overlap(0x10000, 0xC000, 0x4000, 0xF000, 0x1000));
    assert(gart_ranges_overlap(0x10000, 0xF000, 0x1000, 0xC000, 0x4000));
    assert(!gart_ranges_overlap(0x10000, 0xB000, 0x1000, 0xC000, 0x4000));
    assert(!gart_ranges_overlap(0x10000, 0xC000, 0x1000, 0xD000, 0x1000));
    assert(!gart_ranges_overlap(16384, UINT64_MAX, 1, 0, 4096));
    uint64_t reserved_end = 0;
    assert(gart_checked_bump_reserved_end(16384, 12288, 12288, &reserved_end));
    assert(reserved_end == 16384); // HIGH mode reserves the whole upper tail
    assert(gart_checked_bump_reserved_end(16384, 12288, 14336, &reserved_end));
    assert(reserved_end == 16384); // still reserved after partial consumption
    assert(gart_checked_bump_reserved_end(16384, 0, 4096, &reserved_end));
    assert(reserved_end == 4096); // LOW mode protects only the used prefix
    assert(!gart_checked_bump_reserved_end(16384, 8192, 4096, &reserved_end));
    assert(!gart_checked_bump_reserved_end(16384, 0, 16385, &reserved_end));
    assert(!gart_checked_bump_reserved_end(16384, 0, 0, nullptr));
    assert(gart_checked_pte_range(4096, 0, 512));
    assert(gart_checked_pte_range(4096, 511, 1));
    assert(!gart_checked_pte_range(4096, 512, 1));
    assert(!gart_checked_pte_range(4096, UINT64_MAX, 1));
    assert(!gart_checked_pte_range(4095, 0, 1));
    assert(!gart_checked_pte_range(4096, 0, 0));
    uint64_t first_byte = 0;
    assert(gart_checked_pte_vram_range(0x1000, 4096, 511, 1, &first_byte));
    assert(first_byte == 0x1ff8);
    assert(!gart_checked_pte_vram_range(UINT64_MAX - 3ULL, 4096,
                                        0, 1, &first_byte));
    assert(!gart_checked_pte_vram_range(0x1000, 4096, 512, 1, &first_byte));
    assert(!gart_checked_pte_vram_range(0x1000, 4096, 0, 1, nullptr));
}

static void test_atomic_bump_reservations(void)
{
    constexpr uint32_t kThreads = 8;
    constexpr uint32_t kPerThread = 64;
    constexpr uint64_t kSpan = 4096;
    constexpr uint64_t kTotal = (uint64_t)kThreads * kPerThread * kSpan;
    uint64_t next = 0;
    std::vector<uint64_t> offsets[kThreads];
    std::thread workers[kThreads];
    for (uint32_t t = 0; t < kThreads; ++t) {
        workers[t] = std::thread([&, t]() {
            offsets[t].reserve(kPerThread);
            for (uint32_t i = 0; i < kPerThread; ++i) {
                uint64_t offset = 0;
                assert(gart_atomic_bump_reserve(&next, kTotal, kSpan, &offset));
                offsets[t].push_back(offset);
            }
        });
    }
    for (auto &worker : workers) worker.join();

    std::vector<uint64_t> all;
    all.reserve((size_t)kThreads * kPerThread);
    for (const auto &list : offsets) all.insert(all.end(), list.begin(), list.end());
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i < all.size(); ++i)
        assert(all[i] == (uint64_t)i * kSpan);
    assert(next == kTotal);

    uint64_t unused = 0;
    assert(!gart_atomic_bump_reserve(&next, kTotal, kSpan, &unused));
    assert(!gart_atomic_bump_reserve(&next, kTotal, 0, &unused));
    uint64_t misaligned_next = 1;
    assert(!gart_atomic_bump_reserve(&misaligned_next, kTotal, kSpan, &unused));
    assert(!gart_atomic_bump_reserve(&next, kTotal, 1024, &unused));
    assert(!gart_atomic_bump_reserve(nullptr, kTotal, kSpan, &unused));
    assert(!gart_atomic_bump_reserve(&next, kTotal, kSpan, nullptr));
}

static void test_bus_and_add_overflow(void)
{
    uint64_t value = 0;
    assert(gart_checked_bus_span(0x1000, 4096));
    assert(gart_checked_bus_span(UINT64_MAX - 4095ULL, 4096));
    assert(!gart_checked_bus_span(UINT64_MAX - 4095ULL, 8192));
    assert(!gart_checked_bus_span(0, 4096));
    assert(!gart_checked_bus_span(UINT64_MAX - 1024, 4096));
    assert(gart_checked_add_u64(0x1000, 0x2000, &value));
    assert(value == 0x3000);
    assert(!gart_checked_add_u64(UINT64_MAX - 3, 4, &value));
    assert(!gart_checked_add_u64(0, 1, nullptr));
    uint32_t vmid_bit = 0;
    assert(gart_checked_vmid_bit(0, &vmid_bit) && vmid_bit == 1U);
    assert(gart_checked_vmid_bit(15, &vmid_bit) && vmid_bit == 0x8000U);
    assert(!gart_checked_vmid_bit(16, &vmid_bit));
    assert(!gart_checked_vmid_bit(UINT32_MAX, &vmid_bit));
    assert(!gart_checked_vmid_bit(0, nullptr));
}

int main(void)
{
    test_page_spans();
    test_ranges_and_pte_bounds();
    test_atomic_bump_reservations();
    test_bus_and_add_overflow();
    puts("GART checked bounds: 4 test groups passed");
    return 0;
}
