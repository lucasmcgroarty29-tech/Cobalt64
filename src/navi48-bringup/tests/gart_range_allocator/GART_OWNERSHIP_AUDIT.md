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
The top-level kext Makefile's src/amd/*.cpp wildcard compiles this module into the kext, but no live GART code calls it.

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
QUIESCENT -> PTE_CLEAR_WRITTEN -> HDP_FLUSHED -> TLB_INVALIDATED -> REUSABLE`.
The caller supplies a non-zero context mask identifying every hub/VMID pair
that may cache the mapping. It counts in-flight uses, blocks new uses after
retirement begins, and supports a fail-closed `QUARANTINED` state. It does not
execute any hardware operations and is not connected to the kext. Callers must
serialize its state changes.

Run its host suite with `make -C src/navi48-bringup/tests/gart_range_allocator test`.
The suite includes deterministic reuse/coalescing, invalid-input and metadata
capacity checks, plus 50,000 randomized allocate/free operations with full
aperture accounting. The GitHub Actions host-test workflow has run successfully
on the updated allocator and lifecycle code, with AddressSanitizer and
UndefinedBehaviorSanitizer enabled. It reports four allocator test groups and
three lifecycle test groups passed. This does not compile or validate the kext.

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


## Checked mapping arithmetic (2026-11-17)

The active GMC bind helpers and the legacy GART bind helpers now share
`gart_range_bounds.h`, a pure checked-arithmetic layer. Before writing PTEs,
the bind paths validate page-size rounding, the 32-bit PTE-count limit, the
GART aperture interval, the physical bus-address span, the page-table index
range, the absolute page-table VRAM byte range, and the resulting MC address.
Zero-sized and overflowing requests fail before PTE writes. The legacy
`gart_bind_sysmem` path also frees its newly allocated `SysMem` block if its
returned bus span fails validation.

The host suite covers rounding boundaries, zero and overflowing sizes,
out-of-range intervals, PTE-table bounds, absolute VRAM-offset overflow,
physical-address alignment and span overflow, checked addition, and VMID bit indices (0–15 only). The live TLB flush helper now rejects VMIDs outside that range before constructing its request mask, avoiding an unchecked shift. It runs
under AddressSanitizer and UndefinedBehaviorSanitizer. The workflow now runs
these host checks when either mapping source file or the checked-arithmetic
header changes.

This is a defensive source change, not proof of a native kext build. The
host workflow does not compile the IOKit translation units, does not test
the BAR0 write path, and does not validate hardware register behaviour.
The existing concurrency, PTE retirement, hub teardown and Apple mapping
ownership questions remain open. In particular, this change does not add
locking to the shared bump allocator and must not be interpreted as making
concurrent mapping safe.


## Runtime caller synchronization follow-up (2026-11-17)

The active bump allocator has at least one externally entered caller that is
not covered by the fast-copy lock:

- `Navi48Ttl::queryCSBInfo()` calls `navi48_csb_publish()`. That callback
  lazily allocates `gCsbMem`, calls `gmc_bind_existing()`, and sets the
  `gCsbPublished` flag. No lock is taken in the visible callback or publisher.
- Fast-copy 63's staging bind and fast-copy 89's read-back bind call
  `gmc_bind_existing()` while holding `gFastCopyLock`, as documented in
  `Navi48Bringup.cpp`.
- `gFastCopyLock` does not serialize the TTL clear-state publisher. Therefore
  the source currently does not demonstrate one common lock covering every
  runtime bump allocation. A concurrent TTL callback and fast-copy enable
  could race on `gart_bump_offset`; this is a source-level race candidate,
  not a reproduced failure.

The bump allocator now reserves intervals with an atomic compare/exchange
loop, so concurrent `gmc_bind_existing()` callers cannot receive the same
range. Every runtime read of `gart_bump_offset` in the audited GMC and
fast-copy paths uses an atomic load; the initial value is still set before
runtime entry points are published. Host tests run eight threads through
simultaneous page-aligned reservations and verify every interval is unique.

Exact-offset `gmc_bind_at()` now requires HIGH mode and refuses the full
reserved upper aperture. LOW mode is rejected because its bump allocator and
Apple exact mappings would otherwise share one future-allocatable interval.
This avoids pretending that checking only the current high-water mark can
reserve an arbitrary exact-offset mapping. The remaining open questions are
serialization of concurrent exact-offset mappings within Apple's lower
region, and whether concurrent PTE writes/HDP flushes need additional ordering.
This is source-level hardening, not native build or hardware validation.


## Shared BAR and VRAM range guards (2026-11-17)

A related bounds issue existed in the common register helpers:
BAR5 register accessors, BAR0/BAR2 accessors and bulk BAR0 copy/set helpers checked ranges using
`offset + length > mapped_size`. That addition can wrap before the comparison.
They now use a shared subtraction-based `amdgpu_range_fits` check, which
requires `offset <= total` and `length <= total - offset`. The same guard
bounds MM_INDEX VRAM dword reads/writes against the full VRAM size, and
`vram_memcpy` validates the requested span before either the BAR0 or MM_INDEX
path. BAR0 copy tails are written byte-by-byte instead of using a final
32-bit store that could overrun a short request. Non-zero bulk copies reject
a null source pointer. The generic helper is host-tested through the GART
bounds suite.

This change hardens common memory-access bounds but is still only source-level
review plus host tests of the arithmetic primitive. The IOKit register header
and hardware accessors are not compiled by the Linux host workflow, and no
BAR access has been exercised on a GPU.


## VRAM allocator synchronization and bounds (2026-11-17)

A second allocator audit found that `VRAMBumpAllocator` still carried a
DriverKit-era assumption that external-method calls were serialized per
IOService. This fork is an IOKit kext with multiple entry paths, including
`Navi48UserClient::externalMethod()` and Apple TTL callbacks. The allocator
had no internal synchronization, while runtime code has multiple allocation
and release call sites. It now uses a short internal atomic spin lock around
initialization, allocation, free-list mutation and metadata getters. The lock
covers allocator bookkeeping only; it performs no GPU, BAR, fence or memory
allocation operation. Known call sites are thread-context paths, not interrupt
handlers. The host suite now runs eight concurrent threads allocating and
freeing from one allocator and verifies that all returned intervals are
non-overlapping and accounting returns to zero.

The same allocator audit also added overflow-safe initialization, alignment
validation, checked size rounding, range-checked frees, overlap/double-free
rejection, and bounds-safe free-list coalescing. Host tests cover invalid
alignment, wrapping base/size, huge allocation requests, coalescing, duplicate
free, out-of-range free, and concurrent access.

The common range helper has also been applied to SDMA writeback and doorbell
bounds, firmware payload slicing, MES firmware payload ranges, and PSP
firmware/VRAM span checks. These source changes are covered by the host
arithmetic suite, but the Linux workflow still does not compile the IOKit
translation units or validate any hardware path. The separate GMC GART bump
allocator remains a different structure and still has the unsynchronized
runtime reservation issue documented above; the VRAM allocator lock does not
solve it.


## High-reservation overlap correction (2026-11-17)

The exact-offset GMC bind path previously rejected overlap only with the
already-consumed bump interval `[gart_bump_start, gart_bump_offset)`. In
HIGH mode, the whole upper aperture `[gart_bump_start, gart_size)` is reserved
for the driver's bump allocator, including the unused portion before its first
allocation. An exact-offset mapping into that unused tail could therefore
claim space intended for later driver allocations. The overlap check now
protects the entire reserved HIGH interval. In legacy LOW mode
(`gart_bump_start == 0`), it preserves the old behaviour and protects only
the already-consumed `[0, gart_bump_offset)` interval. Host tests cover overlap,
reverse-order overlap, adjacency and invalid ranges. This is still an
unintegrated source-level correction pending native build and hardware testing.


## GART inspection helper bounds (2026-11-17)

The read-only PTE inspection path also used `gart_start + gart_size` and
`page_table_offset + index * 8` before validating the resulting address.
It now validates the aperture-relative offset without an overflowing end
addition, checks the PTE index and absolute VRAM byte range with the shared
helper, and verifies the final 8-byte BAR0 span. The multi-page GART read/write
helpers now validate the entire requested range against the aperture and
reject an absolute end-address overflow before iterating page by page.
These checks do not map or modify GPU memory; they only make the existing
inspection and transfer guards fail closed on malformed ranges.


## Bind API call-site inventory and TLB failure handling (2026-11-17)

A source scan of the AMD C++ directory and the Apple/bring-up glue found the
active runtime mapping calls in `gmc_bind_existing()`: initialization paths
for PDB0, CP, MES and SDMA, plus the CSB publisher and fast-copy staging/read
back paths in `Navi48Bringup.cpp`. The legacy `GARTContext` APIs
(`gart_bind_sysmem`, `gart_bind_existing`, `gart_unbind`) have definitions
but no direct in-tree callsites in the scanned C++ sources. Likewise,
`gmc_bind_at()` is exposed through `navi48_gart_bind_at()`, but no direct
in-tree caller was found in the scanned glue. This is not proof that external
or dynamically resolved callers do not exist; do not delete those APIs on
that basis.

The call-site scan found one more ignored TLB result in native S1b. That path
programs a non-default GFXHUB VMID and previously discarded the return from
`gmc_flush_gpu_tlb()` before running VM legs. It now latches the sequence
stopped if the invalidate does not acknowledge, so the native VM legs are not
run against an unconfirmed translation-cache state.

No automatic TLB flush was added to the bind helpers. The right flush scope
depends on which hub and VMID can cache each mapping; the current call sites
do not encode one universal context mask. The separate bind/retire lifecycle
model remains the place to define that contract before live PTE reclamation.


## Atomic GMC bump reservation (2026-11-17)

The runtime caller race described above has been addressed for bump
allocations: `gmc_bind_existing()` now uses an atomic compare/exchange
reservation primitive with page-alignment and aperture-bound checks. A failed
page-table bounds check after reservation deliberately leaves that interval
quarantined rather than attempting an unsafe rollback while other callers may
already have reserved later ranges. The exact-offset path uses atomic
high-water reads and is disabled in LOW mode; in HIGH mode it cannot overlap
the reserved upper aperture. The host suite includes an eight-thread,
512-reservation stress test and boundary tests for misaligned state and spans.


## CherryRed Phase 52 integration boundary

The supplied CherryRed Phase 52 material documents a host-side GPU virtual
address free-range allocator and a lifecycle model, but did not include the
allocator implementation source. No CherryRed code has therefore been copied
or claimed as integrated. Its VA allocator is also a different address domain
from Cobalt64's GART aperture and page-table ownership: the two cannot be
substituted without translating reservation semantics, page granularity,
Apple-owned exact offsets, hub/VMID visibility, and retirement rules. The
current Cobalt64 work uses the Phase 52 material as a design reference only.


## Concurrency sanitizer result

The new ThreadSanitizer CI step passed on the GART atomic reservation and VRAM
allocator concurrency tests. The latest completed run at this checkpoint ran
all four host suites under AddressSanitizer/UndefinedBehaviorSanitizer and
then reran the two concurrent suites under ThreadSanitizer, with no reported
data races. This validates the host-side reservation and allocator models
only; it does not compile or exercise the IOKit/GPU code that calls them.


## VRAM allocation ownership and geometry hardening (2026-11-17)

The visible-VRAM free-list now has a fixed 1024-entry allocation ledger in
addition to its free-range list. Allocation reserves a ledger record before
changing the free list; free requires an exact match of GPU address, rounded
size, and alignment to a live record. This rejects partial frees and forged
subranges that could otherwise release part of a still-live allocation.
Ledger exhaustion fails closed, and a host test verifies the 1024-record limit
can be recovered after all allocations are returned. The ledger is metadata
only; it does not track GPU fences or prove that a BO is no longer in use.
Callers must still ensure GPU work has completed before freeing a BO.

VRAM/GART MC geometry now uses checked addition and checked power-of-two
alignment before publishing `vram_end`, `gart_start`, and `gart_end`.
Visible and high VRAM pool intervals are checked before allocator init.
Partial visible/high-pool initialisation is retryable: an already-initialised
visible pool no longer causes an early success return that silently skips a
high pool which failed on the previous attempt. The pure ReBAR pool sizing
helper now returns an empty interval rather than wrapping its base or reserve
arithmetic.

Host tests cover normal 256 MiB and 1 GiB pool layouts, overflow cases,
partial/forged frees, allocation-ledger exhaustion and recovery, and existing
concurrent allocation tests. These do not compile the IOKit translation unit;
the checked GMC call-site edits remain source-audited only until a native
macOS build is available.


The allocator also refuses `init()` while live allocations remain, preventing a
reset from orphaning issued BOs and making their VRAM available for reuse.
Reset remains possible once all tracked allocations have been returned. This
is covered by a host regression test.


## PTE write aperture preflight

The mapping APIs write page-table entries through the BAR0 VRAM aperture.
Their page-table allocation bounds alone did not prove that the full PTE span
was currently CPU-writable through the mapped BAR0 window. GMC bump, GMC
exact-offset, and the legacy GART bind paths now preflight the complete PTE
byte span against `dev.bar0` and `dev.bar0Size` before writing entries.
This prevents the low-level void BAR0 copy helper from silently rejecting an
out-of-aperture PTE write while the mapping API reports success. In the bump
path, a failed preflight after interval reservation leaves the interval
quarantined rather than attempting a potentially unsafe rollback.

This remains a preflight against the CPU-visible BAR0 aperture, not a hardware
write acknowledgement and not a GPU TLB invalidation. Mapping retirement,
GPU-use fences, and post-publication visibility still require separate
hardware-aware sequencing.


## BAR0 bulk-copy alignment correction (2026-11-17)

A review of the low-level BAR0 helpers found that their prefix loops advanced
by four bytes while trying to reach an eight-byte boundary. For addresses with
odd low alignment bits, a four-byte stride cannot reach that boundary, so
subsequent typed 32-bit/64-bit stores could remain misaligned. The prefix now
uses byte stores until the destination is eight-byte aligned, after which the
existing aligned bulk stores and bounded tail remain in place. The memset
prefix preserves the repeating four-byte pattern by selecting the correct
pattern byte for each prefix position.

The host workflow does not compile `amdgpu_regs.h` because it depends on
IOKit. CI passing after this source change therefore confirms that the
existing host suites remain green, not that the BAR0 helper itself has been
compiled or exercised. Native macOS compilation and hardware validation
remain outstanding.


## HDP flush acknowledgement gap (2026-11-17)

The BAR0 PTE writers call `amdgpu_hdp_flush()` after publishing entries, but
that helper currently returns `void`. If neither candidate NBIO remap register
contains a plausible flush-window offset, it logs that HDP flush writes are
skipped and still performs only the read-back barrier. The mapping functions
then continue to publish success. The BAR0 preflight proves the CPU can address
the PTE bytes; it does not prove the GPU-visible HDP flush occurred.

This is a distinct gap from TLB invalidation. A future change should first
inventory every `amdgpu_hdp_flush()` callsite and determine which callers can
safely fail when the remap is unavailable. Only then should the helper expose
a checked result and mapping publication propagate failure. Do not blindly
change its return contract or add a generic TLB flush: some callsites are
initialisation/diagnostic paths, and the required hub/VMID scope is not encoded
at this layer.


## Checked HDP flush availability (2026-11-17)

The HDP helper now returns a status instead of silently treating a missing NBIO
remap window as success. A true result means the remapped flush write was
issued and the read-back register access was valid; it is not proof of GPU
completion and does not replace TLB invalidation.

The status is propagated through GMC initialization, PDB0 publication/revert,
GMC bump mappings, and exact-offset mappings. A failed bump-map flush leaves
the already-reserved interval quarantined; it is deliberately not rolled back.
The clear-state publisher and the pre-handoff graphics takeover path also
refuse to publish/hand off after an unavailable flush. Native S1b host-driven
test setup, staging, and poison operations now stop when the flush cannot be
issued.

Other direct flush calls remain in diagnostic, scanout, fast-copy, and
experimental paths. They need individual caller review before broad
propagation because some are best-effort observations while others precede
engine submissions. The helper's status is an availability/issue check only:
the remap write/read-back has not been validated on physical hardware. The
Linux host workflow does not compile these IOKit translation units.


## SDMA submission gating follow-up (2026-11-17)

The checked HDP status is now also enforced at the submission boundary for
the scanout linear-copy path, tiled-copy path, and fast-copy callback. If the
remap window cannot be used, the packet is not written to the ring or kicked.
The fast-copy positive controls release their temporary VRAM scratch when the
pre-submit flush is unavailable; the already-bound system-memory GART buffers
remain retained on bind failure.

This closes the immediate false-success path for these reviewed submissions.
Other diagnostic and scanout experiment callsites still contain best-effort
flush calls and require a separate per-function review. No physical-device
flush acknowledgement or native kext build has been demonstrated.


## Host-tested HDP remap selection policy (2026-11-17)

The NBIO remap-window selection logic is now isolated in the pure `hdp_flush_policy.h` helper and covered by a dedicated host test. Cases cover base-index-2 preference, base-index-0 fallback, zero/sentinel values, misalignment, aperture overflow, the exact final valid dword, and a null output pointer. The IOKit helper uses this same selection function.

These tests validate only the address-selection policy. They do not execute MMIO writes, prove that the remap register is correctly programmed on Navi48, or establish hardware flush completion.


## Native cleanup and GART publication quarantine (2026-11-17)

A second call-site audit found that native BO retirement ignored the boolean result of the HDP flush before checking VMID invalidation. Normal BO release now requires both an issued/available HDP flush and an acknowledged VMID TLB invalidation. If either fails, the descriptor and backing allocation are intentionally retained instead of returning VRAM, GTT, or host-import memory to a reusable pool.

Session close now clears fence-window PTEs and requires HDP plus VMID-0 invalidation BEFORE releasing BO backing. If that sequence is not acknowledged, the slot's fence window is permanently marked poisoned and the session's BO backing is retained. Future opens of that slot are refused rather than reusing potentially stale translations.

The low-level GART sysmem bind path now consumes the bump range and retains its sysmem allocation if HDP publication cannot be issued. Existing-bus binding refuses success on the same failure. For reused slots, caller ownership of the new bus range remains with the caller; the old binding metadata is not overwritten.

These changes are conservative leak/quarantine behavior, not a reclamation implementation. They reduce unsafe reuse but do not prove GPU completion or hardware correctness. Native IOKit translation units still require an actual macOS build and Navi48 hardware validation.


## Native ring and CP/SDMA publication gates (2026-11-17)

The native S1c shared GFX ring now stages packet dwords first and checks HDP before committing its software write pointer, emitted sequence number, owner record, writeback pointer, or doorbell. If the checked flush is unavailable, the packet remains outside the published ring range and the caller receives NotReady.

The SDMA external queue kick paths now check HDP before writing either the BAR2 doorbell or the MMIO write pointer. CP queue publication now checks HDP before marking GFX/compute MQDs ready, before MES ADD_QUEUE for the GFX/compute queues, and in the central CP doorbell helper. The direct compute queue submit path stages its write pointer locally and leaves the committed pointer unchanged if HDP fails.

These gates address the reviewed submission boundaries only. Remaining direct flush calls in diagnostic, readback, scanout experiments, initialization and CP scratch-test paths still require per-caller classification. A checked true result means the remap write/readback path was available; it is not a GPU completion fence. The host CI does not compile these IOKit sources.


## Native VM map and open-path HDP failure handling (2026-11-17)

The native session open path now refuses to publish a new fence window, park root, or session root when the checked HDP flush is unavailable. A newly allocated park root is returned to the VRAM pool only because it has not yet been installed in a GPU context; a failed fence-window initialization permanently poisons that reserved slot range.

Host-import mapping and ordinary VA MAP now retain the BO and software mapping record if HDP or TLB acknowledgement fails, so a later BO release cannot mistakenly recycle the backing memory. VA UNMAP now removes the software map record only after both HDP and VMID invalidation succeed. If either step fails, the map record remains as a quarantine reference and backing memory cannot be safely returned.

The remaining native S1c direct flushes include diagnostic/test setup, ring scrub and other paths that need per-operation review. These edits remain uncompiled IOKit source changes; host CI cannot validate their C++ or runtime behavior.


## SDMA queue-0 publication follow-up (2026-11-17)

The primary SDMA queue path had two additional unchecked boundaries: `sdma_ring_write()` advanced its committed write pointer after an unchecked HDP flush, and `sdma_kick_doorbell()` could publish that pointer through BAR2 or MMIO even when the flush window was unavailable. Ring writes now return failure without advancing the committed pointer, and the queue-0 kick refuses both publication paths unless the checked flush succeeds. All in-tree `sdma_ring_write()` callers compare the returned dword count and therefore stop on this failure.

The queue-1 external kick paths were separately gated in the prior change. These checks prevent new queue publication on the reviewed paths when HDP is unavailable; they do not validate the hardware flush semantics or prove completion of previously submitted work.


## Fence-target and VRAM-zeroing fail-closed paths (2026-11-17)

The native GTT fence-target path now requires both the checked HDP flush and VMID-0 TLB invalidation before recording a fence slot as usable. If either acknowledgement fails, the slot's entire fence window is poisoned; further GTT fence-target resolution through that window is refused, and session close must retire the window before releasing BO backing.

The native visible-VRAM BO creation path also no longer reports a newly allocated BO as zero-initialised when its post-memset HDP flush is unavailable. It retains the allocation as a boot-lifetime quarantine, marks the BO non-releasable, withholds the ZEROED flag, and returns NotReady.

The G1 test-only setup now checks its VMID mapping flush before continuing, and its CPU release control reports failure if the HDP flush cannot be issued. These are conservative failure paths; no native compilation or hardware test has been run.


## Poisoned fence-window close correction (2026-11-17)

A follow-up review found that a failed new fence-target publication can write a PTE before the software `FenceSlot` record is committed. If the flush then fails, the window is marked poisoned but that slot is not present in the session's slot table. Close now treats a poisoned window as requiring full-window retirement: it clears every PTE in the 16-page window and requires HDP plus VMID-0 invalidation before releasing any BO backing. If that acknowledgement fails, backing remains quarantined and the slot stays permanently dead.

This closes a software bookkeeping gap in the fail-closed path. It remains source-level reasoning pending native compilation and hardware validation.


## GMC VMFrag and initialization HDP propagation (2026-11-17)

The VMFrag diagnostic's per-leg source/destination staging now stops before TLB invalidation or SDMA submission if HDP cannot be issued. Its CONTEXT1 restore path treats an unavailable HDP flush exactly like a failed TLB acknowledgement and quarantines the temporary table/data allocations rather than freeing them under uncertain translations.

GFXHUB GART initialization now propagates the HDP result and refuses to publish readiness before the VMID-0 TLB invalidation if the flush window is unavailable. The PDB0 self-test refuses its SDMA copy when its source staging flush fails, and the compute-ring self-test now propagates the checked GMC HDP status before submitting its IB.


## SDMA and CP pointer-publication ordering follow-up (2026-11-17)

The SDMA queue-0 and queue-1 kick paths now preflight HDP before writing a writeback pointer that the engine may poll, then re-check the flush result before issuing the doorbell/MMIO publication path. The primary SDMA ring writer also leaves its committed software pointer unchanged if the ring-content flush is unavailable.

The central CP doorbell helper now drains staged ring writes before updating the writeback WPTR shadow. The direct compute-queue submit path similarly stages its next pointer locally, checks HDP, and only then commits the pointer and rings the doorbell. This avoids publishing a new WPTR shadow before the checked ring-content flush.

The remaining unchecked HDP calls in CP/SDMA test paths are followed by these central checked submission gates, or are readback/diagnostic operations. The SDMA VRAM writeback read helper still has a best-effort flush before a diagnostic read and should be converted to an explicit invalid-read sentinel in a later pass.


## Diagnostic scratch retention and stale-read handling (2026-11-17)

The PDB0 self-test now retains its temporary visible-VRAM destination allocations if the checked source-staging HDP flush fails. It refuses the SDMA copy and does not return those destinations to the allocator, avoiding reuse while CPU-written data may not have been drained. Its GART test page remains retained by design because the bump mapping has no unbind path.

The SDMA VRAM writeback read helper now returns its invalid-read sentinel when HDP cannot be issued, rather than returning a potentially stale BAR0 value as if it were a valid observation.


## CP ring software-pointer rollback (2026-11-17)

The CP ring callers now restore their pre-submit software WPTR if the checked central doorbell gate refuses publication. The multi-part IB + EOP path also rolls back if either packet staging or the final kick fails; the EOP helper checks that its fence packet was actually staged. The ring-fetch probe and KIQ smoke path restore their starting pointer on kick failure. This prevents a later successful submission from accidentally publishing commands from an earlier call that returned NotReady.

`cp_ring_write()` remains a staging helper and does not itself perform HDP I/O; the central `cp_kick_doorbell()` checks the ring-content flush before writing the polled WPTR shadow or doorbell. The direct compute-queue path has its own local next-WPTR transaction and checked pre-publication flush.


## SDMA writeback helper and VMFrag table-staging follow-up (2026-11-17)

`sdma_wb_write32()` now reports invalid offsets or missing CPU mappings instead of silently returning. VRAM writes are staged without an unchecked internal HDP flush; each queue-publication path checks both writeback dwords and then performs its checked HDP drain before doorbell/MMIO publication. Fence-slot initialisation callers also propagate mapping failure before staging/submitting work.

The VMFrag table builder now returns a status and refuses to run either sub-table or 64 KiB-leaf tests when the page-table write flush is unavailable. If a retry table rebuild fails, later legs are skipped. CONTEXT1 restoration still performs a checked HDP + VMID-1 invalidation, and the temporary table/data allocations remain quarantined if that restore acknowledgement fails.


## Queue-1 scratch-ring failure cleanup (2026-11-17)

The Queue-1 self-test now refuses to program the external queue if its fence writeback slot cannot be staged or if the scratch-ring HDP drain fails. In either case it retains the scratch-ring allocation rather than returning memory whose pending BAR0 writes may not have been acknowledged. The test reports NotReady and leaves QUEUE1 unprogrammed.


## Unconfirmed DMA completion and scratch-allocation ownership (2026-11-17)

The SDMA VRAM-copy test no longer reads or frees source/destination allocations after a non-successful fence wait; a failed wait does not prove the engine has stopped accessing them, so both ranges are retained. The multi-case VRAM sweep similarly aborts and retains its whole region when a copy does not complete, or when an HDP staging/readback acknowledgement is unavailable.

The VMFrag diagnostic now treats any non-successful SDMA IB completion as uncertain ownership: it skips readback, does not rewrite the page table for a polarity retry or later leg, restores CONTEXT1, and retains the table/data allocations even if the restore TLB invalidation succeeds. A failed restore invalidation continues to retain the allocations as before. This is deliberately conservative until a positively acknowledged queue reset can prove the engine no longer references the ranges.


## SDMA copy-test ownership after timeout (2026-11-17)

The VRAM-copy test now retains both source and destination allocations whenever the fence wait does not complete successfully; a timeout or failed submission is not proof that DMA has stopped. The multi-case sweep also stops and retains its region on any non-successful copy instead of reading potentially active memory and recycling it. Staging and both readback flush points are checked, and an unacknowledged flush aborts the diagnostic with its region retained.

The SDMA writeback accessor now has a checked boolean result in both declaration and definition. All callers check invalid offsets/missing mappings; queue WPTR writes are staged first and followed by the existing checked HDP drain before publication. The SRBM leg correctly returns `false` on a failed fence-slot write rather than converting a nonzero IOReturn into boolean success.


## VMFrag timeout keeps the test context stable (2026-11-17)

The VMFrag path now also avoids restoring CONTEXT1 when an SDMA IB's completion is unconfirmed. Changing VMID-1's page-table base while the engine may still be walking that context could redirect an in-flight command into unrelated address space. In this exceptional path, the diagnostic leaves CONTEXT1 on the retained test table and keeps both allocations pinned; restoration is deferred until a positively acknowledged queue reset can establish that the engine is idle. This is a deliberate fail-stop state, not a successful cleanup.


## PSP, RLC and compute staging HDP propagation (2026-11-17)

PSP command-ring submission now checks HDP before advancing its WPTR. TOC, TOC sub-bin and register-list staging refuse their PSP command when the payload flush fails. The PSP firmware-buffer bump allocator consumes and quarantines a slot after a failed flush instead of allowing a later stage to overwrite potentially pending bytes.

RLC no longer marks its clear-state buffer populated until the CSB HDP drain succeeds. The compute-test path now checks the HDP result after shader, kernarg, result, IB and IB-test-target staging and stops before verification/submission when the drain is unavailable; its partially staged VRAM allocations remain owned rather than being recycled.


## Remaining SDMA diagnostic staging flushes (2026-11-17)

The SRBM self-test now checks the HDP drain after staging an indirect-buffer packet. If that drain is unavailable, it stops the target's remaining legs, restores the register it temporarily cleared, and retains the IB allocation rather than freeing memory whose staged writes may still be pending. Its initial IB clear is also checked and quarantines the allocation on failure.

The VMID-specific IB copy helper checks the drain before publishing an INDIRECT_BUFFER packet. VMFrag's caller treats any non-successful helper result conservatively: it skips readback and later legs, and retains the table/data backing while leaving CONTEXT1 stable. The standalone VRAM-copy test likewise checks its initial source/destination staging drain and retains both allocations if visibility cannot be acknowledged.


## SDMA submission-state quarantine (2026-11-17)

`SDMAInstance` now carries a fail-stop `submission_uncertain` state. A failed ring-data HDP drain, partial/failed writeback WPTR staging, or failed post-WPTR HDP drain marks the instance uncertain. Queue-0 and Queue-1 kick paths refuse further publication, and `sdma_ring_write` refuses further packet staging. The flag is intentionally not cleared by ordinary queue resume/reallocation; only a fresh device initialisation can establish a clean state. This prevents a later successful flush or unrelated submission from accidentally publishing stale ring contents or a partially updated polled WPTR.


## Queue-1 self-test ring lifetime (2026-11-17)

The Queue-1 routing self-test now treats a published WPTR without a confirmed fence as an in-flight ownership condition. It does not issue a second MMIO kick, disable the queue, or free the scratch ring when completion is uncertain. It also verifies that RB_ENABLE and IB_ENABLE read back clear before releasing the ring after a completed test. External queue programming is refused if the instance is already in the fail-stop `submission_uncertain` state.


## Fence timeout propagation and Queue-1 shutdown verification (2026-11-17)

Queue-0 ring, constant-fill, linear-copy and VMID IB-copy helpers now set `submission_uncertain` when a published command fails to produce its fence. This blocks later submissions rather than allowing another packet to be appended behind a command whose completion is unknown. The SRBM diagnostic applies the same rule and retains its IB allocation after a timeout.

The Queue-1 self-test also stops after a successful kick without a fence, or after an uncertain WPTR publication. It does not attempt a second kick or release the scratch ring in those cases. After a completed test, it reads back both RB_ENABLE and IB_ENABLE and only releases the scratch ring when both are confirmed clear.


## SDMA fail-stop lifetime through teardown (2026-11-17)

The instance-level uncertain state is now sticky across ordinary release/resume paths. `sdma_gfx_resume_instance` refuses to restart an uncertain instance, and `sdma_release_storage` refuses to return its ring or writeback backing to the allocator or OS. This preserves the backing through driver teardown rather than assuming that disabling a queue or timing out a fence proves DMA has stopped. A fresh device initialisation is required to establish a clean instance state.


## CP and MEC queue ownership after uncertain completion (2026-11-17)

The GFX CP ring and MES-mapped compute queues now carry sticky `submission_uncertain` state. A ring-fetch, scratch-register, EOP-fence or compute-fence timeout blocks further submissions and prevents `cp_release_storage` / `cp_compute_queue_release` from returning ring, MQD, EOP or writeback backing to the allocator. Queue setup and MES remapping are refused after uncertainty; an unacknowledged MES ADD_QUEUE is also treated as uncertain ownership. The legacy CPInit path now propagates a ring-fetch timeout instead of reporting the ring ready.

Several CP and native GFX paths had extra unchecked HDP flushes immediately before helpers that already perform a checked drain before publishing WPTR. Those redundant calls were removed so there is one authoritative checked publication barrier. The PDB0 self-test likewise relies on its checked `fill_via_copy` drain for both the destination poison and source pattern. If SDMA completion becomes uncertain in any PDB0 leg, it stops before readback, later legs or CONTEXT0 restoration and retains the VRAM/sysmem backing while leaving the test context stable.


## CP queue mapping ownership and release guards (2026-11-17)

CP and MEC queue storage is also retained while its MES mapping is still active, even if the most recent fence completed. `cp_release_storage` refuses to free the GFX ring/MQD/WB while `kgq_mapped` remains true; `cp_compute_queue_release` similarly refuses while `cq.mapped` is true. A failed MES ADD_QUEUE acknowledgement marks the relevant queue state uncertain, because the firmware may have applied the mapping even when the host did not receive a clean acknowledgement.


## PDB0 inconclusive staging is not a failed mapping verdict (2026-11-17)

The PDB0 diagnostic now distinguishes a failed SDMA completion from an unavailable HDP staging drain. In either case it stops before reading a destination that may be stale, before running later legs, and before reverting CONTEXT0 on an inconclusive result. The per-leg VRAM destination is retained if its poison/pattern staging was not acknowledged; the GART sysmem page remains bound as before. This prevents an HDP access failure from being misreported as evidence that PDB0 address translation is broken.


## Prevent mutation of already-mapped CP queue state (2026-11-17)

The GFX and MEC queue paths now refuse to rewrite an MQD or issue a duplicate MES ADD_QUEUE while the corresponding queue is already mapped. This complements the release guards: mapped storage remains owned until an acknowledged unmap clears the mapping state, and uncertain mappings remain quarantined.


## SDMA warm-start queue stop acknowledgement (2026-11-17)

Before SDMA storage is re-used during initialisation, QUEUE0 RB_ENABLE and IB_ENABLE must both read back clear after the stop writes. If either remains set, initialisation marks the instance uncertain and aborts before allocating/reusing ring backing. A previously uncertain instance is also refused at the start of `sdma_init_full`; the software `enabled` flag is cleared only after the hardware disable is acknowledged.


## Armed legacy CP ring and active SDMA storage lifetime (2026-11-17)

The legacy CP_RB0 path now records when RB_ACTIVE and its doorbell have been armed. That backing is not released or reinitialised while the legacy ring remains armed, and the legacy and MES-mapped GFX paths cannot be mixed on the same CPContext. SDMA storage release is likewise refused while the software instance still reports its queue enabled, even if no explicit fence timeout was recorded.


## Final SDMA SRBM and legacy CP fail-stop details (2026-11-17)

The SRBM self-test no longer restores its target register after a command may still be active. It restores immediately when ring staging fails before WPTR publication, and after a successful fence; it leaves the register untouched when WPTR publication or completion is uncertain, while retaining the IB backing. This avoids racing a late SDMA write with the diagnostic's cleanup write.

The legacy CPInit path now returns any failed ring-fetch probe result rather than continuing to a misleading `CP ready` log. The host workflow has been widened to all AMD driver sources and includes a static guard against standalone unchecked HDP flush calls and unchecked SDMA writeback calls, followed by allocator/lifecycle tests and ThreadSanitizer.


## Fail-stop entry gates for follow-on diagnostics (2026-11-17)

After `submission_uncertain` is set, SDMA writeback writes, ring tests, constant-fill/copy/IB-copy helpers, VRAM copy tests, sweeps and SRBM diagnostics now refuse entry before allocating or staging additional work. VMFrag similarly skips before allocating a second test table. The PM4 bring-up caller now stops immediately when the initial CP ring-fetch probe has an unconfirmed completion, rather than attempting another ring test against a potentially active queue.
