#include "amdgpu_vram.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <algorithm>
#include <thread>
#include <vector>

using amdgpu::VRAMAllocation;
using amdgpu::VRAMBumpAllocator;

static void test_init_and_allocation_guards(void)
{
    VRAMBumpAllocator a;
    a.init(UINT64_MAX - 15ULL, 32ULL);
    assert(!a.is_inited());
    assert(a.bytes_free() == 0);
    a.init(0x100000ULL, 0x10000ULL);
    assert(a.is_inited());

    VRAMAllocation out{};
    assert(!a.alloc(0, 16384, &out));
    assert(!a.alloc(1, 0, &out));
    assert(!a.alloc(1, 3, &out));
    assert(!a.alloc(UINT64_MAX, 16384, &out));
    assert(!a.alloc(0x20000, 16384, &out));

    assert(a.alloc(1, 4096, &out));
    assert(out.gpu_va == 0x100000ULL);
    assert(out.size == 16384ULL);
    assert(out.alignment == 16384ULL);
    assert(a.bytes_used() == 16384ULL);
    a.free(out);
    assert(a.bytes_used() == 0);
    assert(a.bytes_free() == 0x10000ULL);
}

static void test_coalescing_and_double_free(void)
{
    VRAMBumpAllocator a;
    a.init(0x200000ULL, 0x10000ULL);
    VRAMAllocation first{}, middle{}, last{}, whole{};
    assert(a.alloc(0x4000, 0x4000, &first));
    assert(a.alloc(0x4000, 0x4000, &middle));
    assert(a.alloc(0x4000, 0x4000, &last));
    assert(a.bytes_used() == 0xC000ULL);

    a.free(first);
    assert(a.bytes_used() == 0x8000ULL);
    a.free(first); // duplicate release must not change accounting
    assert(a.bytes_used() == 0x8000ULL);
    a.free(last);
    assert(a.bytes_used() == 0x4000ULL);
    a.free(middle);
    assert(a.bytes_used() == 0);
    assert(a.bytes_free() == 0x10000ULL);

    assert(a.alloc(0x10000, 0x4000, &whole));
    assert(whole.size == 0x10000ULL);
    assert(a.bytes_free() == 0);
    a.free(whole);
    assert(a.bytes_free() == 0x10000ULL);
}

static void test_reject_out_of_range_free(void)
{
    VRAMBumpAllocator a;
    a.init(0x300000ULL, 0x8000ULL);
    VRAMAllocation valid{};
    assert(a.alloc(0x4000, 0x4000, &valid));
    const uint64_t used = a.bytes_used();

    VRAMAllocation outside{};
    outside.gpu_va = UINT64_MAX - 7ULL;
    outside.size = 16ULL;
    a.free(outside);
    assert(a.bytes_used() == used);

    outside.gpu_va = 0x2FFFFFULL;
    outside.size = 0x4000ULL;
    a.free(outside);
    assert(a.bytes_used() == used);

    VRAMAllocation malformed = valid;
    malformed.size = 0x2000ULL; // not a whole aligned allocation
    a.free(malformed);
    assert(a.bytes_used() == used);
    malformed = valid;
    malformed.alignment = 3;
    a.free(malformed);
    assert(a.bytes_used() == used);
    malformed = valid;
    malformed.gpu_va += 4096;
    a.free(malformed);
    assert(a.bytes_used() == used);

    a.free(valid);
    assert(a.bytes_used() == 0);
}

static void test_concurrent_allocations_and_frees(void)
{
    constexpr uint32_t kThreads = 8;
    constexpr uint32_t kPerThread = 32;
    constexpr uint64_t kBlock = 16384;
    constexpr uint64_t kPoolBytes = (uint64_t)kThreads * kPerThread * kBlock * 2ULL;
    VRAMBumpAllocator a;
    a.init(0x400000ULL, kPoolBytes);

    std::vector<VRAMAllocation> allocations[kThreads];
    std::thread workers[kThreads];
    for (uint32_t t = 0; t < kThreads; ++t) {
        workers[t] = std::thread([&, t]() {
            allocations[t].reserve(kPerThread);
            for (uint32_t i = 0; i < kPerThread; ++i) {
                VRAMAllocation block{};
                assert(a.alloc(kBlock, kBlock, &block));
                allocations[t].push_back(block);
            }
        });
    }
    for (auto &worker : workers) worker.join();

    std::vector<VRAMAllocation> all;
    all.reserve((size_t)kThreads * kPerThread);
    for (const auto &list : allocations)
        all.insert(all.end(), list.begin(), list.end());
    std::sort(all.begin(), all.end(), [](const VRAMAllocation &x,
                                         const VRAMAllocation &y) {
        return x.gpu_va < y.gpu_va;
    });
    for (size_t i = 1; i < all.size(); ++i)
        assert(all[i - 1].gpu_va + all[i - 1].size <= all[i].gpu_va);
    assert(a.bytes_used() == (uint64_t)kThreads * kPerThread * kBlock);

    for (uint32_t t = 0; t < kThreads; ++t) {
        workers[t] = std::thread([&, t]() {
            for (const auto &block : allocations[t]) a.free(block);
        });
    }
    for (auto &worker : workers) worker.join();
    assert(a.bytes_used() == 0);
    assert(a.bytes_free() == kPoolBytes);
}

int main(void)
{
    test_init_and_allocation_guards();
    test_coalescing_and_double_free();
    test_reject_out_of_range_free();
    test_concurrent_allocations_and_frees();
    puts("VRAM allocator: 4 test groups passed");
    return 0;
}
