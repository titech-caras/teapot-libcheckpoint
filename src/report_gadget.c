#include "report_gadget.h"
#include "checkpoint.h"
#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <sys/mman.h>

#if defined(__x86_64__)
static uint64_t report_saved_r11 LIBCHECKPOINT_PROTECTED_SECTION;

static inline void preserve_report_scratch_registers(void) {
    asm volatile("movq %%r11, %0" : "=m"(report_saved_r11) :: "memory");
}

static inline void restore_report_scratch_registers(void) {
    asm volatile("movq %0, %%r11" :: "m"(report_saved_r11) : "r11", "memory");
}
#elif defined(__aarch64__)
static uint64_t report_saved_x16 LIBCHECKPOINT_PROTECTED_SECTION;
static uint64_t report_saved_x17 LIBCHECKPOINT_PROTECTED_SECTION;

static inline void preserve_report_scratch_registers(void) {
    uint64_t x16;
    uint64_t x17;
    asm volatile(
        "mov %0, x16\n"
        "mov %1, x17"
        : "=r"(x16), "=r"(x17));
    report_saved_x16 = x16;
    report_saved_x17 = x17;
}

static inline void restore_report_scratch_registers(void) {
    uint64_t x16 = report_saved_x16;
    uint64_t x17 = report_saved_x17;
    asm volatile(
        "mov x16, %0\n"
        "mov x17, %1"
        :
        : "r"(x16), "r"(x17)
        : "x16", "x17");
}
#elif defined(__riscv) && __riscv_xlen == 64
static uint64_t report_saved_t0 LIBCHECKPOINT_PROTECTED_SECTION;
static uint64_t report_saved_t1 LIBCHECKPOINT_PROTECTED_SECTION;

static inline void preserve_report_scratch_registers(void) {
    uint64_t t0;
    uint64_t t1;
    asm volatile(
        "mv %0, t0\n"
        "mv %1, t1"
        : "=r"(t0), "=r"(t1));
    report_saved_t0 = t0;
    report_saved_t1 = t1;
}

static inline void restore_report_scratch_registers(void) {
    uint64_t t0 = report_saved_t0;
    uint64_t t1 = report_saved_t1;
    asm volatile(
        "mv t0, %0\n"
        "mv t1, %1"
        :
        : "r"(t0), "r"(t1)
        : "t0", "t1");
}
#else
static inline void preserve_report_scratch_registers(void) {
}

static inline void restore_report_scratch_registers(void) {
}
#endif

void make_report_call_nop(uint64_t gadget_addr) {
    uint64_t page_aligned_addr = gadget_addr & ~(4096UL - 1);
    if (mprotect((void*)page_aligned_addr, 8192, PROT_READ | PROT_WRITE | PROT_EXEC) == -1) {
        perror("mprotect");
        return;
    }

    size_t patch_size = 0;
#if defined(__x86_64__)
    // NOP DWORD ptr [EAX + EAX*1 + 00H]
    *(uint32_t*)gadget_addr = 0x00441f0f;
    *(uint8_t*)(gadget_addr + 4) = 0x00;
    patch_size = 5;
#elif defined(__aarch64__)
    *(uint32_t*)gadget_addr = 0xd503201f;
    patch_size = 4;
#elif defined(__riscv) && __riscv_xlen == 64
    if ((*(uint16_t*)gadget_addr & 0x3) != 0x3) {
        // C.NOP for compressed report calls.
        *(uint16_t*)gadget_addr = 0x0001;
        patch_size = 2;
    } else {
        *(uint32_t*)gadget_addr = 0x00000013;
        patch_size = 4;
    }
#else
    (void)gadget_addr;
#endif

    if (patch_size != 0) {
        __builtin___clear_cache((char *)gadget_addr, (char *)(gadget_addr + patch_size));
    }

    if (mprotect((void*)page_aligned_addr, 8192, PROT_READ | PROT_EXEC) == -1)
        perror("mprotect");
}

void report_gadget(const char * gadget_desc, int gadget_type, uint64_t gadget_addr, uint64_t access_addr, dift_tag_t tag) {
    if (!libcheckpoint_enabled)
        return;

    simulation_statistics.total_bug++;
    simulation_statistics.bug_type[gadget_type]++;

    fprintf(stderr,
            "[teapot], %d %s, 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%x, %" PRIu64 ", ",
            gadget_type,
            gadget_desc,
            gadget_addr,
            access_addr,
            (unsigned int)tag,
            instruction_cnt);

    for (size_t i = checkpoint_cnt; i > 0; i--) {
        fprintf(stderr, "0x%" PRIx64 ", ", checkpoint_metadata[i - 1].return_address);
    }
    fputc('\n', stderr);

#ifdef SILENCE_GADGET_AFTER_FIRST_DISCOVERY
    make_report_call_nop(gadget_addr);
#endif
}

#define DEF_REPORT_GADGET(TYPE) \
    void report_gadget_##TYPE(uint64_t gadget_addr, uint64_t access_addr, dift_tag_t tag) { \
        preserve_report_scratch_registers(); \
        report_gadget(STR(TYPE), GADGET_##TYPE, gadget_addr, access_addr, tag); \
        restore_report_scratch_registers(); \
    }

LIBCHECKPOINT_PRESERVE_MOST DEF_REPORT_GADGET(KASPER_CACHE);
LIBCHECKPOINT_PRESERVE_MOST DEF_REPORT_GADGET(KASPER_MDS);
LIBCHECKPOINT_PRESERVE_MOST DEF_REPORT_GADGET(KASPER_PORT);

#undef DEF_REPORT_GADGET
