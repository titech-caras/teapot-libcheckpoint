#include "checkpoint.h"

#include <cpuid.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

extern void report_gadget_KASPER_CACHE(
    uint64_t gadget_addr, uint64_t access_addr, dift_tag_t tag);
extern int checkpoint_x64_xsave_report_probe(uint64_t gadget_addr);

/* Normal instrumented links provide these coverage-section boundaries. */
uint32_t __guard_start__teapot__[1];
uint32_t __guard_end__teapot__[1];

static uint64_t enabled_vector_xsave_mask(void) {
    unsigned int eax, ebx, ecx, edx;
    const uint64_t vector_components =
        (1ULL << 0) | (1ULL << 1) | (1ULL << 2) |
        (1ULL << 5) | (1ULL << 6) | (1ULL << 7);

    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) ||
            (ecx & bit_XSAVE) == 0 || (ecx & bit_OSXSAVE) == 0) {
        return 0;
    }

    uint32_t xcr0_lo;
    uint32_t xcr0_hi;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    uint64_t mask = (((uint64_t)xcr0_hi << 32) | xcr0_lo) & vector_components;
    if ((mask & 0x3) != 0x3) {
        return 0;
    }

    size_t required_size = 576;
    for (unsigned int component = 2; component <= 7; component++) {
        if ((mask & (1ULL << component)) == 0) {
            continue;
        }
        __cpuid_count(0x0d, component, eax, ebx, ecx, edx);
        size_t component_end = (size_t)ebx + (size_t)eax;
        if (component_end > required_size) {
            required_size = component_end;
        }
    }
    return required_size <= PROCESSOR_EXTENDED_STATE_SIZE ? mask : 0;
}

int main(void) {
    uint64_t mask = enabled_vector_xsave_mask();
    if (mask == 0) {
        fputs("SKIP: OSXSAVE vector state is unavailable\n", stderr);
        return 77;
    }

    /*
     * This is the reserved part of the XSAVE header at the old report-save
     * location. A report implementation must not depend on these scratch-stack
     * bytes remaining zero.
     */
    memset(scratchpad + SCRATCHPAD_SIZE - 8192 + 520, 0xa5, 56);

    void *report_site = mmap(
        NULL, 8192, PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (report_site == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    memset(report_site, 0x90, 8192);

    processor_xsave_mask = mask;
    checkpoint_cnt = 0;
    libcheckpoint_enabled = true;
    int preserved = checkpoint_x64_xsave_report_probe((uintptr_t)report_site);
    libcheckpoint_enabled = false;

    static const unsigned char report_nop[] = {0x0f, 0x1f, 0x44, 0x00, 0x00};
    bool patched = memcmp(report_site, report_nop, sizeof(report_nop)) == 0;
    if (munmap(report_site, 8192) != 0) {
        perror("munmap");
        return 2;
    }

    if (!preserved) {
        fputs("report wrapper did not preserve XMM state\n", stderr);
        return 1;
    }
    if (!patched) {
        fputs("gadget reporter did not execute the full report path\n", stderr);
        return 1;
    }
    return 0;
}
