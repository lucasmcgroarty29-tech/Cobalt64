#include "gart_mapping_lifecycle.h"
#include <assert.h>
#include <stdio.h>

static void normal_lifecycle_requires_each_context() {
    GARTMappingLifecycle l{};
    gart_lifecycle_init(&l, 0x3u); // two hub/VMID contexts may cache this mapping
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
    assert(!gart_lifecycle_note_tlb_invalidated(&l, 0x1u)); // HDP first
    assert(gart_lifecycle_hdp_flushed(&l));
    assert(gart_lifecycle_note_tlb_invalidated(&l, 0x1u));
    assert(l.state == GART_MAPPING_HDP_FLUSHED);
    assert(!gart_lifecycle_retire_complete(&l)); // second context not acked
    assert(gart_lifecycle_note_tlb_invalidated(&l, 0x2u));
    assert(l.state == GART_MAPPING_TLB_INVALIDATED);
    assert(gart_lifecycle_retire_complete(&l));
    assert(l.state == GART_MAPPING_REUSABLE);
    assert(gart_lifecycle_pte_written(&l));
    assert(l.invalidated_context_mask == 0);
    assert(gart_lifecycle_valid(&l));
}

static void rejects_skipped_steps_and_unknown_contexts() {
    GARTMappingLifecycle l{};
    gart_lifecycle_init(&l, 0x3u);
    assert(!gart_lifecycle_mapping_visible(&l));
    assert(!gart_lifecycle_ptes_cleared(&l));
    assert(!gart_lifecycle_retire_complete(&l));
    assert(gart_lifecycle_pte_written(&l));
    assert(!gart_lifecycle_ptes_cleared(&l));
    assert(gart_lifecycle_mapping_visible(&l));
    assert(gart_lifecycle_begin_retire(&l));
    assert(gart_lifecycle_ptes_cleared(&l));
    assert(!gart_lifecycle_note_tlb_invalidated(&l, 0x4u));
    assert(!gart_lifecycle_retire_complete(&l));
    assert(gart_lifecycle_hdp_flushed(&l));
    assert(gart_lifecycle_note_tlb_invalidated(&l, 0x3u));
    assert(gart_lifecycle_retire_complete(&l));
}

static void quarantine_is_fail_closed() {
    GARTMappingLifecycle l{};
    gart_lifecycle_init(&l, 0x1u);
    assert(gart_lifecycle_pte_written(&l));
    assert(gart_lifecycle_mapping_visible(&l));
    assert(gart_lifecycle_begin_use(&l));
    assert(gart_lifecycle_begin_retire(&l));
    assert(gart_lifecycle_quarantine(&l));
    assert(l.state == GART_MAPPING_QUARANTINED && l.in_flight == 1);
    assert(!gart_lifecycle_begin_use(&l));
    assert(!gart_lifecycle_end_use(&l));
    assert(!gart_lifecycle_ptes_cleared(&l));
    assert(!gart_lifecycle_hdp_flushed(&l));
    assert(!gart_lifecycle_note_tlb_invalidated(&l, 0x1u));
    assert(!gart_lifecycle_retire_complete(&l));
    assert(!gart_lifecycle_pte_written(&l));
    assert(gart_lifecycle_valid(&l));
}

int main() {
    normal_lifecycle_requires_each_context();
    rejects_skipped_steps_and_unknown_contexts();
    quarantine_is_fail_closed();
    puts("GART mapping lifecycle: 3 test groups passed");
    return 0;
}
