#include "gart_mapping_lifecycle.h"
#include <assert.h>
#include <stdio.h>
static void normal_lifecycle() {
    GARTMappingLifecycle l{};
    gart_lifecycle_init(&l);
    assert(gart_lifecycle_valid(&l));
    assert(gart_lifecycle_pte_written(&l));
    assert(!gart_lifecycle_begin_use(&l));
    assert(gart_lifecycle_mapping_visible(&l));
    assert(gart_lifecycle_begin_use(&l));
    assert(gart_lifecycle_begin_use(&l));
    assert(l.in_flight == 2);
    assert(gart_lifecycle_begin_retire(&l));
    assert(l.state == GART_MAPPING_RETIRING);
    assert(!gart_lifecycle_begin_use(&l));
    assert(gart_lifecycle_end_use(&l));
    assert(l.state == GART_MAPPING_RETIRING && l.in_flight == 1);
    assert(gart_lifecycle_end_use(&l));
    assert(l.state == GART_MAPPING_QUIESCENT && l.in_flight == 0);
    assert(gart_lifecycle_ptes_cleared(&l));
    assert(gart_lifecycle_retire_complete(&l));
    assert(l.state == GART_MAPPING_REUSABLE);
    assert(gart_lifecycle_pte_written(&l));
    assert(gart_lifecycle_valid(&l));
}
static void rejects_skipped_steps() {
    GARTMappingLifecycle l{};
    gart_lifecycle_init(&l);
    assert(!gart_lifecycle_mapping_visible(&l));
    assert(!gart_lifecycle_ptes_cleared(&l));
    assert(!gart_lifecycle_retire_complete(&l));
    assert(gart_lifecycle_pte_written(&l));
    assert(!gart_lifecycle_ptes_cleared(&l));
    assert(gart_lifecycle_mapping_visible(&l));
    assert(gart_lifecycle_begin_retire(&l));
    assert(gart_lifecycle_ptes_cleared(&l));
    assert(gart_lifecycle_retire_complete(&l));
}
static void quarantine_is_fail_closed() {
    GARTMappingLifecycle l{};
    gart_lifecycle_init(&l);
    assert(gart_lifecycle_pte_written(&l));
    assert(gart_lifecycle_mapping_visible(&l));
    assert(gart_lifecycle_begin_use(&l));
    assert(gart_lifecycle_begin_retire(&l));
    assert(gart_lifecycle_quarantine(&l));
    assert(l.state == GART_MAPPING_QUARANTINED && l.in_flight == 1);
    assert(!gart_lifecycle_begin_use(&l));
    assert(!gart_lifecycle_end_use(&l));
    assert(!gart_lifecycle_ptes_cleared(&l));
    assert(!gart_lifecycle_retire_complete(&l));
    assert(!gart_lifecycle_pte_written(&l));
    assert(gart_lifecycle_valid(&l));
}
int main() {
    normal_lifecycle();
    rejects_skipped_steps();
    quarantine_is_fail_closed();
    puts("GART mapping lifecycle: 3 test groups passed");
    return 0;
}
