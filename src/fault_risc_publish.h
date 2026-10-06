/* Private v4 publication transaction. Included by fault_sites.c only after its
 * raw syscall helper, and by the fault-injection unit. No external callbacks,
 * PLT calls, allocation, diagnostics, or executable/writable page coexistence.
 * Caller proves table/template, page ownership, signal mask and single thread.
 */
#ifndef TEAPOT_PRIVATE_FAULT_RISC_PUBLISH_H
#define TEAPOT_PRIVATE_FAULT_RISC_PUBLISH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#ifndef PROT_BTI
#define PROT_BTI 0x10
#endif

enum fault_risc_patch_result {
    FAULT_RISC_PATCH_REFUSED,
    FAULT_RISC_PATCH_PUBLISHED,
    /* A caller receiving UNSAFE must terminate using a raw path. It must not
     * restore signals or return into application text. */
    FAULT_RISC_PATCH_UNSAFE
};

struct fault_risc_page {
    uintptr_t address;
    int original_protection;
};

/* The normal/copy bounds are the runtime's initialized BTI policy. /proc maps
 * does not report PROT_BTI. A page touching normal text is never patchable.
 * Keep the BTI bit on both transitions, matching aarch64_bti.c's mapping.
 * Complete page isolation is checked by the caller before this helper. */
static bool fault_risc_page_policy(uintptr_t first, uintptr_t page_size,
        int mapping_protection, uintptr_t normal_lo, uintptr_t normal_hi,
        uintptr_t copy_lo, uintptr_t copy_hi, int *protection) {
    if (!protection || !page_size || (page_size & (page_size - 1)) || page_size > 65536 ||
            page_size < 4096 || (first & (page_size - 1)) ||
            first > UINTPTR_MAX - page_size ||
            mapping_protection != (PROT_READ | PROT_EXEC)) return false;
    uintptr_t last = first + page_size;
    if ((normal_lo == 0) != (normal_hi == 0) || (copy_lo == 0) != (copy_hi == 0) ||
            (normal_lo && normal_lo >= normal_hi) || (copy_lo && copy_lo >= copy_hi) ||
            (!!normal_lo != !!copy_lo)) return false;
    if (normal_lo && first < normal_hi && last > normal_lo) return false;
    *protection = mapping_protection;
    if (copy_lo && first < copy_hi && last > copy_lo) *protection |= PROT_BTI;
    return true;
}

#ifndef FAULT_RISC_PUBLISH_TEST
/* These are already runtime-owned exports; no new manifest names. Read the
 * current bounds at publication, because BTI activation follows preinit's
 * ordinary mapping snapshot. Missing/incomplete BTI policy fails closed. */
#if defined(__aarch64__)
extern uint64_t teapot_bti_text_lo __attribute__((weak));
extern uint64_t teapot_bti_text_hi __attribute__((weak));
extern uint64_t teapot_bti_copy_lo __attribute__((weak));
extern uint64_t teapot_bti_copy_hi __attribute__((weak));
#endif
static bool fault_risc_current_page_policy(uintptr_t first, uintptr_t page_size,
                                           int mapping_protection, int *protection) {
    uintptr_t normal_lo = 0, normal_hi = 0, copy_lo = 0, copy_hi = 0;
#if defined(__aarch64__)
    if (&teapot_bti_text_lo || &teapot_bti_text_hi || &teapot_bti_copy_lo || &teapot_bti_copy_hi) {
        if (!&teapot_bti_text_lo || !&teapot_bti_text_hi || !&teapot_bti_copy_lo || !&teapot_bti_copy_hi)
            return false;
        normal_lo = teapot_bti_text_lo; normal_hi = teapot_bti_text_hi;
        copy_lo = teapot_bti_copy_lo; copy_hi = teapot_bti_copy_hi;
        if (!normal_lo || !normal_hi || !copy_lo || !copy_hi) return false;
    }
#endif
    return fault_risc_page_policy(first, page_size, mapping_protection,
                                  normal_lo, normal_hi, copy_lo, copy_hi, protection);
}

static long fault_risc_protect(uintptr_t page, uintptr_t size, int protection) {
    return fault_syscall3(SYS_mprotect, (long)page, (long)size, protection);
}

static bool fault_risc_cache_sync(uintptr_t pc) {
#if defined(__aarch64__)
    uintptr_t ctr;
    __asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t dline = (uintptr_t)4 << ((ctr >> 16) & 15);
    uintptr_t iline = (uintptr_t)4 << (ctr & 15);
    for (uintptr_t line = pc & ~(dline - 1); line < pc + 4; line += dline)
        __asm__ volatile ("dc cvau, %0" :: "r"(line) : "memory");
    __asm__ volatile ("dsb ish" ::: "memory");
    for (uintptr_t line = pc & ~(iline - 1); line < pc + 4; line += iline)
        __asm__ volatile ("ic ivau, %0" :: "r"(line) : "memory");
    __asm__ volatile ("dsb ish\nisb" ::: "memory");
    return true;
#elif defined(__riscv) && __riscv_xlen == 64
    /* Linux asm-generic syscall base + 15; old libc headers can lack the
     * symbolic name. Flags=0 is process-wide, including future migration. */
#ifndef SYS_riscv_flush_icache
#define SYS_riscv_flush_icache 259
#endif
    return fault_syscall3(SYS_riscv_flush_icache, (long)pc, (long)(pc + 4), 0) == 0;
#else
    (void)pc;
    return false;
#endif
}

static void fault_risc_after_rx(void) {
#if defined(__aarch64__)
    __asm__ volatile ("isb" ::: "memory");
#else
    __asm__ volatile ("" ::: "memory");
#endif
}
#endif

static bool fault_risc_restore_pages(const struct fault_risc_page pages[2],
                                     size_t count, uintptr_t page_size) {
    bool ok = true;
    for (size_t i = 0; i < count; ++i)
        if (fault_risc_protect(pages[i].address, page_size, pages[i].original_protection)) ok = false;
    if (ok) fault_risc_after_rx();
    return ok;
}

/* Does not assume aligned word stores, including a RV word at page_end-2.
 * Exact per-page original attributes include BTI, not merely the rwx string.
 */
static enum fault_risc_patch_result fault_risc_publish_word(uintptr_t pc,
        uint32_t original, uint32_t replacement, uintptr_t page_size,
        const struct fault_risc_page pages[2], size_t count) {
    if (!pages || !page_size || (page_size & (page_size - 1)) || page_size < 4096 || page_size > 65536 ||
            (pc & 1) || pc > UINTPTR_MAX - 4 || !count || count > 2) return FAULT_RISC_PATCH_REFUSED;
#if defined(__riscv) && __riscv_xlen == 64
    if ((original & 3) != 3 || (replacement & 3) != 3) return FAULT_RISC_PATCH_REFUSED;
#elif defined(__aarch64__)
    if (pc & 3) return FAULT_RISC_PATCH_REFUSED;
#elif !defined(FAULT_RISC_PUBLISH_TEST)
    return FAULT_RISC_PATCH_REFUSED;
#endif
    uintptr_t first = pc & ~(page_size - 1);
    size_t needed = 1 + ((pc + 3) / page_size - pc / page_size);
    if (count != needed || first > UINTPTR_MAX - count * page_size) return FAULT_RISC_PATCH_REFUSED;
    for (size_t i = 0; i < count; ++i)
        if (pages[i].address != first + i * page_size ||
                (pages[i].original_protection & ~PROT_BTI) != (PROT_READ | PROT_EXEC))
            return FAULT_RISC_PATCH_REFUSED;
    volatile unsigned char *bytes = (volatile unsigned char *)pc;
    for (unsigned i = 0; i < 4; ++i)
        if (bytes[i] != (unsigned char)(original >> (8 * i))) return FAULT_RISC_PATCH_REFUSED;

    size_t writable = 0;
    for (; writable < count; ++writable) {
        int rw = (pages[writable].original_protection & ~PROT_EXEC) | PROT_READ | PROT_WRITE;
        if (fault_risc_protect(pages[writable].address, page_size, rw))
            return fault_risc_restore_pages(pages, writable, page_size) ?
                FAULT_RISC_PATCH_REFUSED : FAULT_RISC_PATCH_UNSAFE;
    }
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (unsigned char)(replacement >> (8 * i));
    __asm__ volatile ("" ::: "memory");
    if (fault_risc_cache_sync(pc) && fault_risc_restore_pages(pages, count, page_size))
        return FAULT_RISC_PATCH_PUBLISHED;

    /* A partial RX restore may have made one page executable already. Obtain
     * RW on every page again before writing any undo byte. Failure is fatal,
     * not a promise that the stale or non-executable application is resumable. */
    for (size_t i = 0; i < count; ++i) {
        int rw = (pages[i].original_protection & ~PROT_EXEC) | PROT_READ | PROT_WRITE;
        if (fault_risc_protect(pages[i].address, page_size, rw)) return FAULT_RISC_PATCH_UNSAFE;
    }
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (unsigned char)(original >> (8 * i));
    __asm__ volatile ("" ::: "memory");
    bool coherent = fault_risc_cache_sync(pc);
    bool executable = fault_risc_restore_pages(pages, count, page_size);
    return coherent && executable ? FAULT_RISC_PATCH_REFUSED : FAULT_RISC_PATCH_UNSAFE;
}
#endif
