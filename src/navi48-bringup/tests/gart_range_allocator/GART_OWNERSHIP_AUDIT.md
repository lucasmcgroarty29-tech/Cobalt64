# GART ownership audit (lucas-development)

## Why the free-range allocator is not wired into the driver yet

Cobalt64 currently has more than one GART-facing path. They do not share
interchangeable ownership semantics.

### 1. `GARTContext` binding helpers

`amdgpu_gart.cpp` implements `gart_bind_sysmem`, `gart_bind_existing` and
`gart_unbind`. Its context currently contains a monotonically increasing
`nextFreeOffset`. `gart_unbind` only releases a `SysMem` allocation owned by
that binding; it does not clear the corresponding PTEs or reclaim the
aperture slot. `gart_bind_existing` also supports reusing a caller-supplied
binding's previous slot.

The new `GARTRangeAllocator` is a possible replacement for the offset bookkeeping
in this path only, after its users, lifetime and synchronization are proven.
It is not currently referenced by the kext build or live GART code.

### 2. `GMCContext` mapping helpers

`gmc_v12_0.cpp` separately implements `gmc_bind_existing` and `gmc_bind_at`.
The former allocates from `gart_bump_offset`; the latter maps a caller-selected
offset and rejects overlap with the driver's reserved bump interval.

The bump interval can start at `gart_bump_start` near the top of the aperture
when the `navi48-gart-high` option is selected. The lower portion is intentionally
left available for Apple's accelerator mappings, which arrive at exact offsets.
A general first-fit allocator must not allocate from this shared aperture
without representing both ownership domains and their reserved intervals.

The PDB0 self-test currently uses `gmc_bind_existing` for its GART round-trip.
That is a distinct path from `GARTContext`.

## Synchronization and reclamation questions still open

The inspected PTE-writing helpers perform an HDP flush after writing entries.
The TLB invalidation routine exists separately as `gmc_flush_gpu_tlb`. The
current helper interfaces do not encode a complete bind/unbind retirement
protocol. Before an offset can be reused, the driver must establish:

1. which hubs and VMIDs may have cached the mapping;
2. when GPU work using the old mapping has completed;
3. how the PTEs are cleared (including partial final pages, if supported);
4. which HDP/cache flush and TLB invalidations are required, and how timeout
   failures prevent reuse;
5. how caller-owned memory differs from `SysMem`-owned memory;
6. how Apple's exact-offset mappings are protected from the driver's allocator.

A failed invalidation must leave the range unavailable. Returning it to a
free list before old translations are retired would create a use-after-free
risk for GPU-visible memory.

## Allocator constraints

The host-side allocator now takes an explicit granularity. For Navi48 GART
aperture offsets this should be `kAMDGPUGPUPageSize` `(4096)`. Allocation size,
free offset and free size must be granularity multiples; alignment must be a
power of two and at least the granularity.

The allocator remains an isolated, deterministic bookkeeping component.
It does not write PTEs, flush HDP, invalidate TLBs, wait for GPU fences, or
manage `SysMem` lifetimes. Those responsibilities must remain in the
driver layer.

Run its host suite with `make -C src/navi48-bringup/tests/gart_range_allocator test`.
The suite includes deterministic reuse/coalescing, invalid-input and metadata
capacity checks, plus 50,000 randomized allocate/free operations with full
aperture accounting. The updated granularity checks have been added to the
suite; they still need a fresh run after this API change.

## Next safe engineering step

Add a host-side lifecycle model that represents the states
`allocated -> mapped -> in-flight -> retired -> reusable` and test failure
paths (especially invalidation timeout). Then map each real caller to those
states. Only after that should the driver integrate a free-list, and only in
the `GARTContext` path if the ownership audit confirms it is still used.
