/*
 * Host-side state gate for GART mapping retirement.
 * Models ordering only; performs no GPU, PTE, fence, or cache operation.
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum GARTMappingState {
    GART_MAPPING_UNMAPPED = 0,
    GART_MAPPING_PTE_WRITTEN,
    GART_MAPPING_MAPPED,
    GART_MAPPING_IN_FLIGHT,
    GART_MAPPING_RETIRING,
    GART_MAPPING_QUIESCENT,
    GART_MAPPING_PTE_CLEAR_WRITTEN,
    GART_MAPPING_HDP_FLUSHED,
    GART_MAPPING_TLB_INVALIDATED,
    GART_MAPPING_REUSABLE,
    GART_MAPPING_QUARANTINED
} GARTMappingState;

typedef struct GARTMappingLifecycle {
    GARTMappingState state;
    uint32_t in_flight;
    uint32_t required_context_mask;
    uint32_t invalidated_hub_mask;
} GARTMappingLifecycle;

/* required_context_mask uses one bit per translation hub that can cache this VA. */
void gart_lifecycle_init(GARTMappingLifecycle *lifecycle,
                         uint32_t required_context_mask);
bool gart_lifecycle_pte_written(GARTMappingLifecycle *lifecycle);
bool gart_lifecycle_mapping_visible(GARTMappingLifecycle *lifecycle);
bool gart_lifecycle_begin_use(GARTMappingLifecycle *lifecycle);
bool gart_lifecycle_end_use(GARTMappingLifecycle *lifecycle);
bool gart_lifecycle_begin_retire(GARTMappingLifecycle *lifecycle);
bool gart_lifecycle_ptes_cleared(GARTMappingLifecycle *lifecycle);
bool gart_lifecycle_hdp_flushed(GARTMappingLifecycle *lifecycle);
bool gart_lifecycle_note_tlb_invalidated(GARTMappingLifecycle *lifecycle,
                                         uint32_t hub_mask);
bool gart_lifecycle_retire_complete(GARTMappingLifecycle *lifecycle);
bool gart_lifecycle_quarantine(GARTMappingLifecycle *lifecycle);
bool gart_lifecycle_valid(const GARTMappingLifecycle *lifecycle);

#ifdef __cplusplus
}
#endif
