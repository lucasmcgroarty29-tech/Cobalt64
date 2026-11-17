# GART free-range allocator (staged)

This is a bounded, host-testable allocator for byte offsets within the GART
aperture. It uses caller-provided descriptor storage, first-fit allocation,
power-of-two alignment, sorted free ranges, and adjacent-range coalescing.
It performs no dynamic allocation and has no dependency on IOKit.

## Deliberate integration boundary

This module is not yet connected to `GARTContext` or the `gart_bind_*` /
`gart_unbind` paths. A range must not be returned to this allocator in the live
driver until the corresponding PTEs have been cleared and the required GPU
translation invalidation / synchronization has completed. The current GART
code only documents an HDP flush; that is not, by itself, proof that stale GPU
translations are invalidated. Binding ownership, synchronization, and
concurrent access also need to be resolved before enabling reuse.

The caller must serialize access to one allocator instance. Its range storage
must remain alive and have the declared capacity for the allocator's lifetime.

## Host tests

From this directory, run:

```sh
make test
```

The test target enables AddressSanitizer and UndefinedBehaviorSanitizer by
default. These tests validate allocator bookkeeping only; they do not test
GART PTE programming, GPU TLB invalidation, IOKit memory ownership, or hardware.
