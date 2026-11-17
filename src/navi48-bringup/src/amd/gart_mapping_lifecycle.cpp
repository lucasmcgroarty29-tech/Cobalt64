#include "gart_mapping_lifecycle.h"
#include <limits.h>
void gart_lifecycle_init(GARTMappingLifecycle *l) {
    if (!l) return;
    l->state = GART_MAPPING_UNMAPPED;
    l->in_flight = 0;
}
bool gart_lifecycle_pte_written(GARTMappingLifecycle *l) {
    if (!l || l->in_flight != 0 ||
        (l->state != GART_MAPPING_UNMAPPED &&
         l->state != GART_MAPPING_REUSABLE)) return false;
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
         l->state != GART_MAPPING_RETIRING)) return false;
    --l->in_flight;
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
    l->state = GART_MAPPING_PTES_CLEARED;
    return true;
}
bool gart_lifecycle_retire_complete(GARTMappingLifecycle *l) {
    if (!l || l->state != GART_MAPPING_PTES_CLEARED) return false;
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
        l->state > GART_MAPPING_QUARANTINED) return false;
    if ((l->state == GART_MAPPING_IN_FLIGHT ||
         l->state == GART_MAPPING_RETIRING) && l->in_flight == 0)
        return false;
    if (l->state != GART_MAPPING_IN_FLIGHT &&
        l->state != GART_MAPPING_RETIRING &&
        l->state != GART_MAPPING_QUARANTINED && l->in_flight != 0)
        return false;
    return true;
}
