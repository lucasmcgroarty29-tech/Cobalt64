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
