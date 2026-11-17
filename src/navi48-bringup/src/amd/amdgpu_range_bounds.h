/*
 * Overflow-safe range validation shared by AMD register and memory helpers.
 * Pure arithmetic only; no hardware access.
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

static inline bool amdgpu_range_fits(uint64_t total_bytes,
                                     uint64_t offset,
                                     uint64_t length)
{
    return offset <= total_bytes && length <= total_bytes - offset;
}

static inline bool amdgpu_checked_add_u64(uint64_t a, uint64_t b,
                                           uint64_t *result)
{
    if (result == nullptr || b > UINT64_MAX - a) return false;
    *result = a + b;
    return true;
}

static inline bool amdgpu_checked_align_up_u64(uint64_t value,
                                                uint64_t alignment,
                                                uint64_t *result)
{
    if (result == nullptr || alignment == 0 ||
        (alignment & (alignment - 1ULL)) != 0)
        return false;
    const uint64_t mask = alignment - 1ULL;
    if (value > UINT64_MAX - mask) return false;
    *result = (value + mask) & ~mask;
    return true;
}
