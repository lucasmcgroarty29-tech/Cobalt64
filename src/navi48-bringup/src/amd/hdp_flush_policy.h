#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pure selection policy for the NBIO-remapped HDP flush window.
// The returned offset is a byte offset within the mapped register aperture.
static inline bool
amdgpu_hdp_flush_window_plausible(uint32_t offset, size_t mapped_bytes) {
    if (offset == 0 || offset == UINT32_MAX || (offset & 3u) != 0)
        return false;
    const size_t byte_offset = (size_t)offset;
    return byte_offset <= mapped_bytes && mapped_bytes - byte_offset >= 4;
}

// Prefer NBIO base-index 2; fall back to base-index 0 only when idx2 is unusable.
// On failure, *out_offset is always cleared to zero.
static inline bool
amdgpu_hdp_flush_select_window(uint32_t idx2, uint32_t idx0,
                               size_t mapped_bytes, uint32_t *out_offset) {
    if (!out_offset) return false;
    *out_offset = 0;
    if (amdgpu_hdp_flush_window_plausible(idx2, mapped_bytes)) {
        *out_offset = idx2;
        return true;
    }
    if (amdgpu_hdp_flush_window_plausible(idx0, mapped_bytes)) {
        *out_offset = idx0;
        return true;
    }
    return false;
}
