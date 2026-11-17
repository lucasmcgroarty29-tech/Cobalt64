/*
 * Checked arithmetic for GART page spans and page-table bounds.
 * Pure host-testable helpers; performs no hardware or memory access.
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "amdgpu_range_bounds.h"

static inline bool gart_checked_page_span(uint64_t size_bytes,
                                          uint64_t *rounded_bytes,
                                          uint32_t *page_count)
{
    const uint64_t page_size = 4096ULL;
    const uint64_t page_mask = page_size - 1ULL;
    if (size_bytes == 0 || rounded_bytes == nullptr || page_count == nullptr)
        return false;
    if (size_bytes > UINT64_MAX - page_mask) return false;

    const uint64_t rounded = (size_bytes + page_mask) & ~page_mask;
    const uint64_t pages = rounded / page_size;
    if (pages == 0 || pages > UINT32_MAX) return false;

    *rounded_bytes = rounded;
    *page_count = (uint32_t)pages;
    return true;
}

static inline bool gart_checked_range(uint64_t total_bytes,
                                      uint64_t offset,
                                      uint64_t span_bytes)
{
    return span_bytes != 0 && amdgpu_range_fits(total_bytes, offset, span_bytes);
}

static inline bool gart_checked_pte_range(uint64_t table_bytes,
                                          uint64_t first_pte,
                                          uint32_t page_count)
{
    if (table_bytes == 0 || (table_bytes & 7ULL) != 0 || page_count == 0)
        return false;
    const uint64_t total_ptes = table_bytes / 8ULL;
    return first_pte <= total_ptes &&
           (uint64_t)page_count <= total_ptes - first_pte;
}

static inline bool gart_checked_bus_span(uint64_t bus_address,
                                         uint64_t rounded_bytes)
{
    return bus_address != 0 &&
           (bus_address & 4095ULL) == 0 &&
           rounded_bytes >= 4096ULL &&
           (rounded_bytes - 1ULL) <= UINT64_MAX - bus_address;
}

static inline bool gart_checked_add_u64(uint64_t base,
                                        uint64_t offset,
                                        uint64_t *result)
{
    if (result == nullptr || offset > UINT64_MAX - base) return false;
    *result = base + offset;
    return true;
}

static inline bool gart_checked_pte_vram_range(uint64_t table_base,
                                               uint64_t table_bytes,
                                               uint64_t first_pte,
                                               uint32_t page_count,
                                               uint64_t *first_byte_offset)
{
    if (first_byte_offset == nullptr ||
        !gart_checked_pte_range(table_bytes, first_pte, page_count))
        return false;
    const uint64_t relative_offset = first_pte * 8ULL;
    const uint64_t span_bytes = (uint64_t)page_count * 8ULL;
    uint64_t first = 0;
    uint64_t end = 0;
    if (!gart_checked_add_u64(table_base, relative_offset, &first) ||
        !gart_checked_add_u64(first, span_bytes, &end))
        return false;
    *first_byte_offset = first;
    return true;
}

static inline bool gart_checked_vmid_bit(uint32_t vmid, uint32_t *bit)
{
    if (bit == nullptr || vmid >= 16U) return false;
    *bit = 1U << vmid;
    return true;
}
