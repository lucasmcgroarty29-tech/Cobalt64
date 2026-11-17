#include "gart_range_bounds.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_page_spans(void)
{
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
    assert(gart_checked_pte_range(4096, 0, 512));
    assert(gart_checked_pte_range(4096, 511, 1));
    assert(!gart_checked_pte_range(4096, 512, 1));
    assert(!gart_checked_pte_range(4096, UINT64_MAX, 1));
    assert(!gart_checked_pte_range(4095, 0, 1));
    assert(!gart_checked_pte_range(4096, 0, 0));
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
}

int main(void)
{
    test_page_spans();
    test_ranges_and_pte_bounds();
    test_bus_and_add_overflow();
    puts("GART checked bounds: 3 test groups passed");
    return 0;
}
