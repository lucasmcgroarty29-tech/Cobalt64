#include "gart_mapping_lifecycle.h"

void gart_lifecycle_init(GARTMappingLifecycle *l, uint32_t required_context_mask) {
    if (!l) return;
    l->state = GART_MAPPING_UNMAPPED;
    l->in_flight = 0;
    l->required_context_mask = required_context_mask;
    l->invalidated_context_mask = 0;
}

bool gart_lifecycle_pte_written(GARTMappingLifecycle *l) {
    if (!l || l->required_context_mask == 0 || l->in_flight != 0 ||
        (l->state != GART_MAPPING_UNMAPPED &&
         l->state != GART_MAPPING_REUSABLE)) return false;
    l->invalidated_context_mask = 0;
    l->state = GART_MAPPING_PTE_WRITTEN;
    return true;
}

bool gart_lifecycle_mapping_visible(GARTMappingLifecycle *l) {
    if (!l || l->state != GART_MAPPING_PTE_WRITTEN) return false;
    l->state = GART_MAPPING_MAPPED;
    return true;
}

bool gart_lifecycle_begin_use(GARTMappingLifecycle *l) {
    if (!l || (l->state != GART_MAPPING_MAPPED &&
               l->state != GART_MAPPING_IN_FLIGHT) ||
        l->in_flight == UINT32_MAX) return false;
    ++l->in_flight;
    l->state = GART_MAPPING_IN_FLIGHT;
    return true;
}

bool gart_lifecycle_end_use(GARTMappingLifecycle *l) {
    if (!l || l->in_flight == 0 ||
        (l->state != GART_MAPPING_IN_FLIGHT &&
         l->state != GART_MAPPING_RETIRING &&
         l->state != GART_MAPPING_QUARANTINED)) return false;
    --l->in_flight;
    if (l->state == GART_MAPPING_QUARANTINED) {
        // A separately confirmed completion may update accounting, but
        // quarantine is terminal: it never becomes reusable through this path.
        return true;
    }
    if (l->in_flight == 0) {
        l->state = (l->state == GART_MAPPING_RETIRING)
            ? GART_MAPPING_QUIESCENT : GART_MAPPING_MAPPED;
    }
    return true;
}

bool gart_lifecycle_begin_retire(GARTMappingLifecycle *l) {
    if (!l || (l->state != GART_MAPPING_MAPPED &&
               l->state != GART_MAPPING_IN_FLIGHT)) return false;
    l->state = (l->in_flight == 0)
        ? GART_MAPPING_QUIESCENT : GART_MAPPING_RETIRING;
    return true;
}

bool gart_lifecycle_ptes_cleared(GARTMappingLifecycle *l) {
    if (!l || l->state != GART_MAPPING_QUIESCENT || l->in_flight != 0)
        return false;
    l->state = GART_MAPPING_PTE_CLEAR_WRITTEN;
    return true;
}

bool gart_lifecycle_hdp_flushed(GARTMappingLifecycle *l) {
    if (!l || l->state != GART_MAPPING_PTE_CLEAR_WRITTEN) return false;
    l->state = GART_MAPPING_HDP_FLUSHED;
    return true;
}

bool gart_lifecycle_note_tlb_invalidated(GARTMappingLifecycle *l,
                                         uint32_t context_mask) {
    if (!l || l->state != GART_MAPPING_HDP_FLUSHED || context_mask == 0 ||
        (context_mask & ~l->required_context_mask) != 0) return false;
    l->invalidated_context_mask |= context_mask;
    if (l->invalidated_context_mask == l->required_context_mask)
        l->state = GART_MAPPING_TLB_INVALIDATED;
    return true;
}

bool gart_lifecycle_retire_complete(GARTMappingLifecycle *l) {
    if (!l || l->state != GART_MAPPING_TLB_INVALIDATED ||
        l->invalidated_context_mask != l->required_context_mask) return false;
    l->state = GART_MAPPING_REUSABLE;
    return true;
}

bool gart_lifecycle_quarantine(GARTMappingLifecycle *l) {
    if (!l || l->state == GART_MAPPING_UNMAPPED ||
        l->state == GART_MAPPING_REUSABLE ||
        l->state == GART_MAPPING_QUARANTINED) return false;
    l->state = GART_MAPPING_QUARANTINED;
    return true;
}

bool gart_lifecycle_valid(const GARTMappingLifecycle *l) {
    if (!l || l->state < GART_MAPPING_UNMAPPED ||
        l->state > GART_MAPPING_QUARANTINED ||
        l->required_context_mask == 0 ||
        (l->invalidated_context_mask & ~l->required_context_mask) != 0)
        return false;
    if ((l->state == GART_MAPPING_IN_FLIGHT ||
         l->state == GART_MAPPING_RETIRING) && l->in_flight == 0)
        return false;
    if (l->state != GART_MAPPING_IN_FLIGHT &&
        l->state != GART_MAPPING_RETIRING &&
        l->state != GART_MAPPING_QUARANTINED && l->in_flight != 0)
        return false;
    if (l->state < GART_MAPPING_HDP_FLUSHED &&
        l->invalidated_context_mask != 0) return false;
    if ((l->state == GART_MAPPING_TLB_INVALIDATED ||
         l->state == GART_MAPPING_REUSABLE) &&
        l->invalidated_context_mask != l->required_context_mask) return false;
    return true;
}
