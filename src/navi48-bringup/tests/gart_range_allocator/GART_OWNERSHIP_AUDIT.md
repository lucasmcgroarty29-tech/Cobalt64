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

## Follow-up audit: hub lifetime and mapping visibility

### Hub lifetime is not yet retired during bring-up teardown

The GMC source contains enable paths for MMHUB and GFXHUB, including programming their page-table bases, aperture geometry, VMID configuration and invalidation engines. The inspected amdgpu_init.cpp teardown stops SDMA, CP/MEC and MES, then releases buffers and calls gmc_release_resources().

No inverse GART-disable routine is called from that teardown. In particular, gmc_release_resources() clears software fields and releases mem_scratch and dummy_page, but does not disable the GART contexts, clear their page-table base/enable registers, or invalidate both hubs before releasing those backing resources. This is a concrete teardown gap to resolve before treating unload, reload, or partial bring-up failure as a fully retired mapping context.

This matters beyond ordinary GART buffers: the hubs' protection-fault configuration can reference the dummy page, and the page-table base remains hardware register state until explicitly changed or reset. Stopping command engines is necessary but is not the same as retiring translation contexts.

### Mapping visibility is not a complete bind contract

gmc_bind_existing() and gmc_bind_at() write PTEs and perform an HDP flush. They do not call gmc_flush_gpu_tlb(). The TLB flush routine itself is bounded and returns kIOReturnTimeout when acknowledgement does not arrive, but bind helpers do not make invalidation part of their success contract.

A fresh mapping may not need invalidation if the driver can prove that no GPU ever queried that VA before the PTE was installed. That proof must be explicit: the address may have been used before, a prior invalid PTE may have been cached, or an exact-offset Apple mapping may be updated. For such cases, a successful HDP flush alone does not prove the translation cache has been retired.

Before exposing a generic mapping API, define whether bind must invalidate the relevant hub/VMID, or require the caller to provide a documented precondition proving the VA is fresh. Propagate every TLB timeout and quarantine the interval on uncertainty. Do not silently ignore the result of a flush.

### Separate GARTContext path must remain disabled from integration work

gart_init() resets GARTContext::nextFreeOffset to zero. The live GMC path can reserve the high region for this driver's buffers while Apple owns exact offsets in the low region. Therefore the legacy gart_bind_sysmem()/gart_bind_existing() bump allocator, if called with this context, would begin allocating at the start of the aperture rather than respecting GMC's reserved high interval or Apple's exact-offset ownership.

The inspected bring-up calls gart_init() after GFXHUB enable, but no direct production call to the legacy gart_bind_sysmem(), gart_bind_existing() or gart_unbind() was found in the inspected call-site set. The GMCContext mapping helpers are the confirmed active path. Treat the legacy GART binding API as dormant and unsafe to activate until it has an explicit shared-aperture partition and owner contract.

### Required teardown design questions

1. What is the safe order for stopping every GPU client, disabling VMID contexts on both hubs, invalidating each relevant VMID on both hubs, and only then freeing system pages and page-table resources?
2. Which registers must be restored or disabled on partial bring-up failure, where only one hub may have been enabled?
3. Can Apple-owned contexts or display-related translations still be active while this kext is unloading? If so, the kext must not globally disable or rewrite those contexts without an ownership handshake.
4. What reset/reload path guarantees stale hardware translations cannot refer to a previous boot's freed host pages?

Until those answers are established, do not implement a generic gmc_unbind() or allocator reuse path. A whole-context teardown and an individual mapping retirement are separate operations and need separate contracts.


## Lifecycle model update

The host-side lifecycle gate now separates writing cleared PTEs, completing the HDP flush, and acknowledging TLB invalidation for every required hub/VMID context. Its tests reject invalidation before the HDP flush, completion before all required context bits are acknowledged, unknown context bits, skipped steps, and reuse after quarantine. This remains a contract model only: it cannot establish which hubs or VMIDs a real allocation actually requires, and it does not issue or verify hardware operations.

## Fail-closed TLB acknowledgement changes

The active initialization paths previously ignored the return value from the initial VMID-0 TLB flush on MMHUB and GFXHUB. The GFXHUB readiness path also marked GART ready after a failed GFXHUB flush, and logged-but-continued if the software GART context could not be initialized. These paths now propagate a failed TLB acknowledgement and refuse to publish GART readiness. GMCInit similarly stops if either applicable VMID-0 hub flush fails.

The optional VMFrag self-test also previously ignored VMID-1 invalidation failures before test submissions and after restoring CONTEXT1. It now refuses a test submission if the pre-submit flush fails. If the restore flush fails, it deliberately retains the temporary page-table and data VRAM allocations rather than returning them to the allocator while stale translations may still exist. The result structure records the failed restore and quarantined allocations.

These are source-level fail-closed changes. The native kext has not been built or run on hardware in this environment. The host CI does not compile these IOKit/kernel translation units, so they still require a separate macOS/Xcode build and careful review before any hardware test.

## Remaining concurrency question: shared bump allocator

GMCContext stores gart_bump_start and gart_bump_offset as ordinary 64-bit fields. The active gmc_bind_existing() path reads the current offset, checks capacity, advances it, and writes PTEs without an evident lock or atomic reservation. gmc_bind_at() checks overlap against the same interval and writes exact-offset PTEs without sharing a serialization primitive.

The visible callers include sequential bring-up allocations (SDMA storage and the PDB0 self-test), plus runtime-facing paths in Navi48Bringup.cpp for the clear-state buffer and fast-copy staging/read-back buffers. The fast-copy functions have their own lock, but that does not automatically serialize them with the clear-state publisher or Apple exact-offset mapping path. The latter may be called from a separate service/hook context; the exact callback threading and lock order still need confirmation.

This creates a potential race: two gmc_bind_existing() calls could reserve the same bump interval, or gmc_bind_at() could check the old bump interval while a concurrent driver allocation is extending it. No concurrency failure has been demonstrated, but the code currently has no obvious shared guard. Before live allocator work, identify every callback's execution context and lock order, then serialize bump reservation and exact-offset overlap checking under one shared sleep-capable lock. Do not add a lock blindly: an unknown callback may already hold a lock that would create an inversion or recursive acquisition.

The lifecycle model assigns mask bits as MMHUB VMIDs 0–15 in bits 0–15 and GFXHUB VMIDs 0–15 in bits 16–31. For example, MMHUB VMID 0 plus GFXHUB VMID 0 is mask 0x00010001. The caller must still determine the actual set of contexts that can cache a given mapping; the model does not infer it.
