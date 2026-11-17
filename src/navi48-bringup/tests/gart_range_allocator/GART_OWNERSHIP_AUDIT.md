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

A separate host-side `GARTMappingLifecycle` helper now models the required
ordering: `UNMAPPED -> PTE_WRITTEN -> MAPPED -> IN_FLIGHT -> RETIRING ->
QUIESCENT -> PTES_CLEARED -> REUSABLE`. It counts in-flight uses, blocks new
uses after retirement begins, and supports a fail-closed `QUARANTINED` state.
It does not execute any of the hardware operations represented by those
transitions and is not yet connected to the kext. Callers must serialize its
state changes.

Run its host suite with `make -C src/navi48-bringup/tests/gart_range_allocator test`.
The suite includes deterministic reuse/coalescing, invalid-input and metadata
capacity checks, plus 50,000 randomized allocate/free operations with full
aperture accounting. The updated granularity checks have been added to the
suite; they still need a fresh run after this API change.

## Next safe engineering step

Map each real caller to the lifecycle states and establish the real fence,
PTE-clear, HDP-flush and TLB-invalidation sequence, including timeout paths.
Then add tests for those caller contracts. Only after that should the driver
integrate a free-list, and only in the `GARTContext` path if the ownership
audit confirms it is still used.

## Production call-site audit (2026-11-17)

The following call sites were confirmed by reading the current branch sources.
This is a source-level audit, not a runtime validation.

| Path | Confirmed use | Ownership / lifecycle consequence |
|---|---|---|
| `amdgpu_init.cpp` PDB0 self-test | Calls `gmc_bind_existing` for one test `SysMem` page | The page is deliberately retained after the test because the bump allocator has no release path and a live PTE must not point at reclaimed memory. |
| `sdma_v7_0.cpp` | Allocates each SDMA writeback page in `SysMem`, maps it with `gmc_bind_existing`, and stores the resulting GART MC address | The SDMA instance owns the host allocation; the GMC bump slot is not individually reclaimed. |
| `Navi48Bringup.cpp` clear-state buffer | Allocates and maps one page with `gmc_bind_existing`, then retains it for the kext lifetime | Explicitly persistent mapping. |
| `Navi48Bringup.cpp` fast-copy staging and read-back buffers | Maps staging allocations with `gmc_bind_existing` | Persistent bump allocations; failures can leave allocated host memory retained for the boot. |
| `Navi48Bringup.cpp` Apple GART mirror wrapper | Converts Apple's absolute GART MC address to an aperture offset and calls `gmc_bind_at` | Exact-offset shared mapping. Must not be moved by a first-fit allocator. |
| `amdgpu_gart.cpp` | Defines `gart_bind_sysmem`, `gart_bind_existing`, and `gart_unbind` | These helpers use the separate `GARTContext::nextFreeOffset` allocator. No production caller was established in the call-site files inspected so far; treat this path as unproven/possibly dormant, not as the confirmed integration target. |

### Important teardown finding

The normal bring-up teardown in `amdgpu_init.cpp` first removes scheduler queues,
stops SDMA queue fetch, halts SDMA engines, disables CP/MEC, and parks MES. It then
calls `sdma_release_storage` and later `gmc_release_resources`.

That ordering provides an engine-stop step before the SDMA writeback `SysMem`
allocation is freed. However, the inspected teardown does not show the full
mapping-retirement sequence required for safe slot reuse: it does not clear the
individual GART PTEs and invalidate the relevant hub TLBs before freeing the
writeback allocation. `gmc_release_resources` itself frees scratch allocations
and drops allocator metadata; it does not itself disable the GART hubs or perform
a TLB flush. This does not prove a live GPU access occurs during teardown, because
the engines are stopped first, but it means teardown must not be treated as a
general-purpose unbind/reuse implementation.

The SDMA storage release path also frees the `SysMem` writeback page while its
GART PTE remains in the page table. That can be acceptable only as part of a
strictly quiesced whole-device teardown where the mapping is never reused and
the hardware page-table context is safely retired before any engine can resume.
The current code's hub-disable / context-retirement guarantees need a separate
audit before claiming that condition is met.

### Consequence for allocator integration

The confirmed active allocator is `GMCContext::gart_bump_offset`, not
`GARTContext::nextFreeOffset`. But the GMC aperture is shared: Apple's mappings
occupy caller-selected offsets in the lower region, while the driver-owned bump
allocator can be placed in a reserved high region. A general allocator cannot
replace the bump pointer until it models both ownership ranges and a reservation
mechanism for Apple's exact-offset mappings.

The immediate safe next step is therefore not to wire in the free-list. It is to
audit the full teardown and each persistent mapping owner, then define a GMC
mapping record that links the aperture interval, host-memory owner, GPU users,
hub/VMID visibility, and retirement status. Any timeout or uncertain engine state
must quarantine the interval rather than return it to the allocator.
