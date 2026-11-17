#include "rebar_pure.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

using n48rebar::Pools;
using n48rebar::pool_sizes;

static void test_normal_pool_geometry()
{
    constexpr uint64_t MiB = 1ULL << 20;
    constexpr uint64_t kBase = 15ULL * MiB;
    constexpr uint64_t kAllocOffset = 24ULL * MiB;
    constexpr uint64_t kReserve = 768ULL * MiB;
    constexpr uint64_t kVram = 16304ULL * MiB;

    const Pools legacy = pool_sizes(kBase, kAllocOffset, 256ULL * MiB,
                                    kVram, kReserve);
    assert(legacy.visBase == 39ULL * MiB);
    assert(legacy.visSize == 217ULL * MiB);
    assert(legacy.hi);
    assert(legacy.hiBase == 256ULL * MiB);
    assert(legacy.hiSize == 15280ULL * MiB);

    const Pools rebar = pool_sizes(kBase, kAllocOffset, 1ULL * 1024 * MiB,
                                   kVram, kReserve);
    assert(rebar.visBase == 39ULL * MiB);
    assert(rebar.visSize == 985ULL * MiB);
    assert(rebar.hi);
    assert(rebar.hiBase == 1024ULL * MiB);
    assert(rebar.hiSize == 14512ULL * MiB);
}

static void test_overflow_and_empty_ranges()
{
    const Pools baseOverflow = pool_sizes(UINT64_MAX - 7ULL, 16ULL,
                                          UINT64_MAX, UINT64_MAX, 0ULL);
    assert(baseOverflow.visBase == 0);
    assert(baseOverflow.visSize == 0);
    assert(!baseOverflow.hi);

    const Pools reserveOverflow = pool_sizes(0, 0, UINT64_MAX - 7ULL,
                                              UINT64_MAX, 16ULL);
    assert(!reserveOverflow.hi);
    assert(reserveOverflow.hiSize == 0);

    const Pools noVisible = pool_sizes(0x1000, 0x2000, 0x3000,
                                       0x100000, 0x1000);
    assert(noVisible.visBase == 0x3000);
    assert(noVisible.visSize == 0);
    assert(noVisible.hi);
    assert(noVisible.hiBase == 0x3000);
    assert(noVisible.hiSize == 0xFC000);
}

int main()
{
    test_normal_pool_geometry();
    test_overflow_and_empty_ranges();
    puts("ReBAR pool geometry: 2 test groups passed");
    return 0;
}
