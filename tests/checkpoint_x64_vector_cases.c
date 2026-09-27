#include <assert.h>
#include <cpuid.h>
#include <stdint.h>
#include "checkpoint.h"

extern void checkpoint_x64_vector_cases_probe(unsigned outer, int inner, unsigned df);
extern uintptr_t checkpoint_target_metadata[];
static uint32_t branch_count;
uint64_t vector_observed[8];
uint32_t __guard_start__teapot__[1], __guard_end__teapot__[1];

/* Also exercise the normal checkpoint path across an ABI-clobbering hook. */
void hfuzz_trace_pc(uint64_t pc) {
    (void)pc;
    __asm__ volatile("pxor %%xmm0, %%xmm0; pxor %%xmm15, %%xmm15" ::: "xmm0", "xmm15");
}

int main(void) {
    unsigned eax, ebx, ecx, edx;
    uint64_t mask = 0, optimized = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & bit_OSXSAVE)) {
        uint32_t lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        mask = (((uint64_t)hi << 32) | lo) & 7;
        __cpuid_count(0x0d, 1, eax, ebx, ecx, edx);
        optimized = eax & 1;
    }
    /* Full -> compact -> full at the same depth must not damage the XSAVEOPT
     * backing image. Cover FXSAVE and XSAVE fallbacks as well as XSAVEOPT. */
    const unsigned profiles[] = {2, 1, 0, 2, 1, 2};
    for (unsigned hardware = 0; hardware < 3; ++hardware) {
        processor_xsave_mask = hardware ? mask : 0;
        processor_has_xsaveopt = hardware == 2 ? optimized : 0;
        libcheckpoint_enabled = 0;
        libcheckpoint_set_vector_state(4);
        libcheckpoint_enabled = 1;
        for (unsigned p = 0; p < sizeof(profiles)/sizeof(profiles[0]); ++p) {
            unsigned outer = profiles[p];
            for (int inner = -1; inner < 3; ++inner) {
                for (unsigned df = 0; df <= 1; ++df) {
                    unsigned saved_outer = TEAPOT_X64_VECTOR_MODE == 1 ? 1 :
                        TEAPOT_X64_VECTOR_MODE > 1 ? 2 : outer;
                    unsigned saved_inner = TEAPOT_X64_VECTOR_MODE == 1 ? 1 :
                        TEAPOT_X64_VECTOR_MODE > 1 ? 2 : (unsigned)inner;
                    checkpoint_target_metadata[CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR / 8] =
                        (uintptr_t)&branch_count;
                    branch_count = 0;
                    checkpoint_x64_vector_cases_probe(outer, inner, df);
                    assert(checkpoint_cnt == 0);
                    if (saved_outer != 0) assert(vector_observed[0] == 10);
                    if (saved_outer == 2) assert(vector_observed[1] == 15);
                    assert(vector_observed[2] == 0x1f80);
                    assert((vector_observed[6] & 0x400) == df * 0x400);
                    if (inner >= 0) {
                        if (saved_inner != 0) assert(vector_observed[3] == 30);
                        if (saved_inner == 2) assert(vector_observed[4] == 35);
                        assert(vector_observed[5] == 0x1f80);
                        assert((vector_observed[7] & 0x400) == df * 0x400);
                    }
                }
            }
        }
    }
    return 0;
}
