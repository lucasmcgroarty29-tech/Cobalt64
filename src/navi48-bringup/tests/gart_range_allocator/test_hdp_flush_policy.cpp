#include "hdp_flush_policy.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void prefers_idx2() {
    uint32_t out = 0;
    assert(amdgpu_hdp_flush_select_window(0x100, 0x200, 0x1000, &out));
    assert(out == 0x100);
}

static void falls_back_to_idx0() {
    uint32_t out = 0;
    assert(amdgpu_hdp_flush_select_window(0xFFFFFFFFu, 0x200, 0x1000, &out));
    assert(out == 0x200);
}

static void rejects_invalid_windows() {
    uint32_t out = 0xDEADBEEFu;
    assert(!amdgpu_hdp_flush_select_window(0, 0xFFFFFFFFu, 0x1000, &out));
    assert(out == 0);
    assert(!amdgpu_hdp_flush_select_window(0x101, 0x203, 0x1000, &out));
    assert(out == 0);
    assert(!amdgpu_hdp_flush_select_window(0x1000, 0x2000, 0x1000, &out));
    assert(out == 0);
    assert(!amdgpu_hdp_flush_select_window(0xFFFFFFFCu, 0, 0x1000, &out));
    assert(out == 0);
    assert(!amdgpu_hdp_flush_select_window(0, 0, 0x1000, nullptr));
}

static void accepts_exact_end_boundary() {
    uint32_t out = 0;
    assert(amdgpu_hdp_flush_select_window(0xFFC, 0, 0x1000, &out));
    assert(out == 0xFFC);
    assert(!amdgpu_hdp_flush_select_window(0xFFD, 0, 0x1000, &out));
    assert(out == 0);
}

int main() {
    prefers_idx2();
    falls_back_to_idx0();
    rejects_invalid_windows();
    accepts_exact_end_boundary();
    puts("HDP flush window policy: 4 test groups passed");
    return 0;
}
