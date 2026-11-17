// VERBATIM from lemonade-sdk/mac-amdgpu (MIT) dext/amdgpu/amdgpu_vram.h @ 3bdeed2 — platform-independent; keep in sync, do not fork.
//
//  amdgpu_vram.h — Allocator for the visible VRAM aperture (BAR0-LOW).
//
//  v0.1.27 upgrade: from "top-down bump, no free" to a first-fit
//  free-list allocator that actually supports free()/coalesce. Used
//  by per-client BO allocation (`MacAMDGPUMethodBOAlloc`) where BOs
//  ship and ship-back across the client lifetime — a no-free bump
//  leaked the whole 232 MB window after the first ~256 BOs were
//  cycled.
//
//  Apple Silicon + TB5 specifics still apply:
//    - On our R9700 setup BAR0 (framebuffer aperture) maps the LOW
//      256 MB of VRAM; allocations from this allocator are guaranteed
//      to live inside that BAR0-mapped window (the allocator is pinned
//      to [vram_start + 24 MB, vram_start + visible_vram_size) by
//      gmc_vram_alloc_init).
//    - PSP-reserved hardcoded slots (fwPri / ring / cmd / fence / TMR /
//      fwBuf) occupy [0..24 MB] of VRAM and are NOT served by this
//      allocator — they're managed at fixed offsets by psp_v14_0.cpp.
//
//  Class name kept as `VRAMBumpAllocator` for source-compat with the
//  pre-existing GMCContext::vram_alloc field and the RLC clear-state
//  setup path (the only in-dext caller pre-v0.1.27). Both the API
//  surface (alloc / free / init / bytes_used / bytes_free) and the
//  return shape (VRAMAllocation { gpu_va, cpu_ptr, size, alignment })
//  are unchanged.
//
//  Algorithm notes:
//    - Singly-linked list of [offset, size) FREE ranges, sorted by
//      offset. The list lives in a fixed-capacity pool of nodes (no
//      kernel allocations in the alloc/free hot path).
//    - First-fit. RDNA4 driver bringup BO sets are tiny (< 1024 BOs)
//      and short-lived; first-fit's O(n) scan is fine.
//    - Coalesce on free: merge with the previous and next free node
//      if adjacent in offset space.
//    - Single-threaded by construction: DriverKit external-method
//      dispatch is serialised per IOService, so a per-client allocator
//      sees no concurrent callers.
//

#pragma once

#include <stdint.h>
#include "amdgpu_range_bounds.h"

namespace amdgpu {

struct VRAMAllocation {
    uint64_t gpu_va;     // GPU-side bus address
    void    *cpu_ptr;    // CPU-side pointer (nullptr if BAR2 not mapped in-dext)
    uint64_t size;
    uint64_t alignment;
};

//
// First-fit free-list allocator over a [base, base+size) range.
// Init signature kept compatible with the pre-v0.1.27 bump allocator
// (init(base, size, cpu_base = nullptr)) so existing callers (RLC
// clear-state, GMCContext::vram_alloc) keep linking unchanged.
//
class VRAMBumpAllocator {
public:
    VRAMBumpAllocator() = default;

    void init(uint64_t base, uint64_t size, void *cpu_base = nullptr) {
        m_inited = false;
        m_base = base;
        m_size = size;
        m_cpu_base = static_cast<uint8_t *>(cpu_base);
        m_bytes_used = 0;
        m_alloc_count = 0;
        m_head = kInvalid;
        for (uint32_t i = 0; i < kMaxNodes; i++) {
            m_pool[i].next = kInvalid;
            m_pool[i].in_use = false;
            m_pool[i].offset = 0;
            m_pool[i].length = 0;
        }
        // Reject an empty or wrapping allocator interval before publishing it.
        if (size == 0 || size > UINT64_MAX - base) return;
        const uint16_t root = pool_take();
        if (root == kInvalid) return;
        m_pool[root].offset = 0;
        m_pool[root].length = size;
        m_pool[root].next = kInvalid;
        m_head = root;
        m_inited = true;
    }

    bool is_inited() const { return m_inited; }
    uint64_t base() const { return m_base; }
    uint64_t size() const { return m_size; }
    uint64_t bytes_used() const { return m_bytes_used; }
    uint64_t bytes_free() const { return m_inited ? m_size - m_bytes_used : 0; }
    uint32_t alloc_count() const { return m_alloc_count; }

    // Allocate. alignment must be a power of two (we coerce up to
    // amdgpu::kASPageSize = 16 KB to keep DART happy on any future
    // re-export of these allocations).
    bool alloc(uint64_t bytes, uint64_t alignment, VRAMAllocation *out) {
        if (!m_inited || bytes == 0 || out == nullptr) return false;
        if (alignment < 16384) alignment = 16384;
        if ((alignment & (alignment - 1ULL)) != 0 ||
            bytes > UINT64_MAX - (alignment - 1ULL))
            return false;
        const uint64_t rounded = (bytes + alignment - 1ULL) & ~(alignment - 1ULL);
        if (rounded == 0 || m_bytes_used > m_size ||
            rounded > m_size - m_bytes_used)
            return false;

        uint16_t prev = kInvalid;
        uint16_t cur = m_head;
        while (cur != kInvalid) {
            FreeNode &n = m_pool[cur];
            if (!amdgpu_range_fits(m_size, n.offset, n.length)) return false;
            if (n.offset > UINT64_MAX - m_base) return false;
            const uint64_t start_abs = m_base + n.offset;
            if (start_abs > UINT64_MAX - (alignment - 1ULL)) {
                prev = cur;
                cur = n.next;
                continue;
            }
            const uint64_t aligned_abs = (start_abs + alignment - 1ULL) &
                                         ~(alignment - 1ULL);
            const uint64_t pad = aligned_abs - start_abs;
            if (rounded > UINT64_MAX - pad) {
                prev = cur;
                cur = n.next;
                continue;
            }
            const uint64_t needed = pad + rounded;
            if (n.length >= needed) {
                const uint64_t alloc_offset = n.offset + pad;
                if (pad == 0 && n.length == rounded) {
                    if (prev == kInvalid) m_head = n.next;
                    else m_pool[prev].next = n.next;
                    pool_release(cur);
                } else if (pad == 0) {
                    n.offset += rounded;
                    n.length -= rounded;
                } else if (n.length == needed) {
                    n.length = pad;
                } else {
                    const uint16_t tail = pool_take();
                    if (tail == kInvalid) return false;
                    m_pool[tail].offset = alloc_offset + rounded;
                    m_pool[tail].length = n.length - needed;
                    m_pool[tail].next = n.next;
                    n.length = pad;
                    n.next = tail;
                }
                m_bytes_used += rounded;
                m_alloc_count++;
                out->gpu_va = m_base + alloc_offset;
                out->size = rounded;
                out->alignment = alignment;
                out->cpu_ptr = m_cpu_base != nullptr
                    ? (m_cpu_base + alloc_offset) : nullptr;
                return true;
            }
            prev = cur;
            cur = n.next;
        }
        return false;
    }

    // Free. Returns the bytes released to the pool, or 0 if the
    // allocation didn't match this allocator's range.
    void free(const VRAMAllocation &a) {
        if (!m_inited || a.size == 0 || a.gpu_va < m_base) return;
        const uint64_t offset = a.gpu_va - m_base;
        const uint64_t length = a.size;
        if (!amdgpu_range_fits(m_size, offset, length) ||
            m_bytes_used > m_size || length > m_bytes_used)
            return;

        uint16_t prev = kInvalid;
        uint16_t cur = m_head;
        while (cur != kInvalid && m_pool[cur].offset < offset) {
            prev = cur;
            cur = m_pool[cur].next;
        }
        const uint64_t end = offset + length;
        if (prev != kInvalid) {
            const FreeNode &n = m_pool[prev];
            if (!amdgpu_range_fits(m_size, n.offset, n.length)) return;
            const uint64_t prev_end = n.offset + n.length;
            if (offset < prev_end) return; // overlap or double-free
        }
        if (cur != kInvalid) {
            const FreeNode &n = m_pool[cur];
            if (!amdgpu_range_fits(m_size, n.offset, n.length)) return;
            if (end > n.offset) return; // overlap or double-free
        }

        bool merged_prev = false;
        if (prev != kInvalid &&
            m_pool[prev].offset + m_pool[prev].length == offset) {
            m_pool[prev].length += length;
            merged_prev = true;
        }
        if (cur != kInvalid && end == m_pool[cur].offset) {
            if (merged_prev) {
                m_pool[prev].length += m_pool[cur].length;
                m_pool[prev].next = m_pool[cur].next;
                pool_release(cur);
            } else {
                m_pool[cur].offset = offset;
                m_pool[cur].length += length;
            }
        } else if (!merged_prev) {
            const uint16_t fresh = pool_take();
            if (fresh == kInvalid) return;
            m_pool[fresh].offset = offset;
            m_pool[fresh].length = length;
            m_pool[fresh].next = cur;
            if (prev == kInvalid) m_head = fresh;
            else m_pool[prev].next = fresh;
        }
        m_bytes_used -= length;
        if (m_alloc_count > 0) m_alloc_count--;
    }

private:
    static constexpr uint16_t kMaxNodes = 256;  // up to 256 free ranges
    static constexpr uint16_t kInvalid  = 0xFFFF;

    struct FreeNode {
        uint64_t offset;
        uint64_t length;
        uint16_t next;
        bool     in_use;
    };

    uint16_t pool_take() {
        for (uint16_t i = 0; i < kMaxNodes; i++) {
            if (!m_pool[i].in_use) {
                m_pool[i].in_use = true;
                return i;
            }
        }
        return kInvalid;
    }
    void pool_release(uint16_t idx) {
        if (idx >= kMaxNodes) return;
        m_pool[idx].in_use = false;
        m_pool[idx].next   = kInvalid;
        m_pool[idx].offset = 0;
        m_pool[idx].length = 0;
    }

    bool      m_inited      = false;
    uint64_t  m_base        = 0;
    uint64_t  m_size        = 0;
    uint64_t  m_bytes_used  = 0;
    uint32_t  m_alloc_count = 0;
    uint8_t  *m_cpu_base    = nullptr;
    uint16_t  m_head        = kInvalid;
    FreeNode  m_pool[kMaxNodes];
};

} // namespace amdgpu
