#include "checkpoint.h"
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__x86_64__)
static uint64_t report_saved_r11 LIBCHECKPOINT_PROTECTED_SECTION;
LIBCHECKPOINT_ASSERT_PROTECTED(report_saved_r11);

static inline void preserve_report_scratch_registers(void) {
    asm volatile("movq %%r11, %0" : "=m"(report_saved_r11) :: "memory");
}

static inline void restore_report_scratch_registers(void) {
    asm volatile("movq %0, %%r11" :: "m"(report_saved_r11) : "r11", "memory");
}
#endif

/* Reporting runs during simulation. Neither libc's allocator nor its ASan
 * interceptors may access this rollback-exempt, poisoned output storage. */
static char report_buffer[4096] LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
static size_t report_output_size LIBCHECKPOINT_PROTECTED_SECTION;
static uint64_t report_maps_warning LIBCHECKPOINT_PROTECTED_SECTION;
LIBCHECKPOINT_ASSERT_PROTECTED(report_buffer);
LIBCHECKPOINT_ASSERT_PROTECTED(report_output_size);
LIBCHECKPOINT_ASSERT_PROTECTED(report_maps_warning);

static void report_write(const char *bytes, size_t size) {
    while (size) {
        ssize_t written = syscall(SYS_write, STDERR_FILENO, bytes, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        bytes += written;
        size -= written;
    }
}

static void report_error(const char *message) {
    size_t size = 0;
    while (message[size])
        size++;
    report_write(message, size);
}

static void report_flush(void) {
    report_write(report_buffer, report_output_size);
    report_output_size = 0;
}

static void report_character(char ch) {
    if (report_output_size == sizeof(report_buffer))
        report_flush();
    report_buffer[report_output_size++] = ch;
}

static void report_text(const char *text) {
    while (*text)
        report_character(*text++);
}

static void report_number(uint64_t value, unsigned base) {
    char digits[20];
    size_t size = 0;
    do {
        digits[size++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value);
    while (size)
        report_character(digits[--size]);
}

static bool report_map_address(const char **cursor, const char *end, uintptr_t *value) {
    const char *start = *cursor;
    *value = 0;
    while (*cursor < end) {
        char ch = **cursor;
        unsigned digit;
        if (ch >= '0' && ch <= '9')
            digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            digit = ch - 'a' + 10;
        else
            break;
        if (*value > (UINTPTR_MAX - digit) / 16)
            return false;
        *value = *value * 16 + digit;
        (*cursor)++;
    }
    return *cursor != start;
}

static bool report_map_prefix(const char *prefix, size_t size, uintptr_t *start,
                              uintptr_t *end, int *protection) {
    const char *limit = prefix + size;
    if (!report_map_address(&prefix, limit, start) || prefix == limit || *prefix++ != '-' ||
            !report_map_address(&prefix, limit, end) || prefix == limit || *prefix++ != ' ' ||
            *start >= *end || limit - prefix < 5)
        return false;
    if ((prefix[0] != 'r' && prefix[0] != '-') ||
            (prefix[1] != 'w' && prefix[1] != '-') ||
            (prefix[2] != 'x' && prefix[2] != '-') ||
            (prefix[3] != 'p' && prefix[3] != 's') || prefix[4] != ' ')
        return false;
    *protection = (prefix[0] == 'r' ? PROT_READ : 0) |
                 (prefix[1] == 'w' ? PROT_WRITE : 0) |
                 (prefix[2] == 'x' ? PROT_EXEC : 0);
    return true;
}

static bool report_page_protections(uintptr_t first_page, size_t page_size,
                                    size_t page_count, int *protections) {
    int fd;
    do {
        fd = syscall(SYS_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY | O_CLOEXEC, 0);
    } while (fd < 0 && errno == EINTR);
    bool success = false;
    if (fd >= 0) {
        /* Retain only the address/permissions prefix, skipping arbitrarily long
         * paths without confusing a read boundary with a line boundary. */
        char prefix[4 * sizeof(uintptr_t) + 8];
        size_t prefix_size = 0;
        while (!success) {
            ssize_t count = syscall(SYS_read, fd, report_buffer, sizeof(report_buffer));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                break;
            bool eof = count == 0;
            if (eof)
                report_buffer[count++] = '\n';
            for (ssize_t pos = 0; pos < count && !success; pos++) {
                if (report_buffer[pos] != '\n') {
                    if (prefix_size < sizeof(prefix))
                        prefix[prefix_size++] = report_buffer[pos];
                    continue;
                }
                uintptr_t start, end;
                int protection;
                if (report_map_prefix(prefix, prefix_size, &start, &end, &protection)) {
                    success = true;
                    for (size_t i = 0; i < page_count; i++) {
                        uintptr_t page = first_page + i * page_size;
                        if (start <= page && page < end)
                            protections[i] = protection;
                        success &= protections[i] >= 0;
                    }
                }
                prefix_size = 0;
            }
            if (eof) {
                /* Unknown pages stay at -1. The RV patcher may discover a
                 * two-byte call whose actual extent needs only the first page. */
                success = true;
                break;
            }
        }
        syscall(SYS_close, fd);
    }
    if (!success && !report_maps_warning) {
        report_maps_warning = true;
        report_error("Could not determine report page permissions; leaving report call unchanged\n");
    }
    return success;
}

#if defined(__aarch64__)
/* AArch64 relaxation can expand a labelled BL into ADRP x16, ADD/LDR x16,
 * BLR x16 while the label still names the sequence's first instruction.
 * Suppress its final call, not the address setup: leaving LDR/BLR behind a
 * NOP would use an unrelated incoming x16 value. Recognize the two exact
 * forms emitted by the rewriter; an ordinary BL (including a linker veneer)
 * and the rewriter's directly labelled BLR remain a single instruction. The
 * three words are read only when every page they occupy is mapped readable,
 * so a label near the end of a mapping cannot fault here. */
static uint64_t expanded_report_call_site(uint64_t gadget_addr, size_t page_size) {
    uint32_t call[3];
    const size_t window = sizeof(call);
    if (gadget_addr > UINTPTR_MAX - (window - 1))
        return gadget_addr;
    uintptr_t first_page = (uintptr_t)gadget_addr & ~(page_size - 1);
    size_t page_count = 1 + (gadget_addr + window - 1 - first_page) / page_size;
    int protections[2] = {-1, -1};
    if (page_count > 2 ||
            !report_page_protections(first_page, page_size, page_count, protections))
        return gadget_addr;
    for (size_t i = 0; i < page_count; i++) {
        if (protections[i] < 0 || !(protections[i] & PROT_READ))
            return gadget_addr;
    }
    /* Report C code runs on a poisoned runtime stack. Even uninstrumented
     * -O0 builds must not call an ASan-intercepted memcpy into that stack. */
    const volatile uint32_t *words = (const volatile uint32_t *)(uintptr_t)gadget_addr;
    for (size_t i = 0; i < sizeof(call) / sizeof(call[0]); i++)
        call[i] = words[i];
    if ((call[0] & 0x9f00001fU) == 0x90000010U &&
        ((call[1] & 0xffc003ffU) == 0x91000210U ||
         (call[1] & 0xffc003ffU) == 0xf9400210U) &&
        (call[2] == 0xd63f0200U || call[2] == 0xd503201fU))
        return gadget_addr + 8;
    return gadget_addr;
}
#endif

void make_report_call_nop(uint64_t gadget_addr) {
#if defined(__x86_64__)
    unsigned char nop[] = {0x0f, 0x1f, 0x44, 0, 0};
#elif defined(__aarch64__)
    unsigned char nop[] = {0x1f, 0x20, 0x03, 0xd5};
#elif defined(__riscv) && __riscv_xlen == 64
    unsigned char nop[] = {0x13, 0, 0, 0};
#else
#error "Unsupported report-call encoding"
#endif
    long size = sysconf(_SC_PAGESIZE);
    if (size < (long)sizeof(nop) || (size & (size - 1)) != 0 ||
            gadget_addr > UINTPTR_MAX - (sizeof(nop) - 1)) {
        report_error("Invalid report patch extent or page size\n");
        return;
    }
    size_t page_size = (size_t)size;
#if defined(__aarch64__)
    gadget_addr = expanded_report_call_site(gadget_addr, page_size);
#endif
    uintptr_t first_page = (uintptr_t)gadget_addr & ~(page_size - 1);
    size_t patch_size = sizeof(nop);
    size_t page_count = 1 + (gadget_addr + patch_size - 1 - first_page) / page_size;
    int protections[2] = {-1, -1};
    if (!report_page_protections(first_page, page_size, page_count, protections))
        return;

    size_t changed_pages = 0;
    bool restore_failed = false;
    for (size_t i = 0; i < page_count; i++) {
        if (protections[i] < 0) {
            report_error("Report patch crosses an unmapped page\n");
            goto restore;
        }
        if (mprotect((void *)(first_page + i * page_size), page_size,
                     protections[i] | PROT_READ | PROT_WRITE) != 0) {
            report_error("Could not make report page writable\n");
            goto restore;
        }
        changed_pages++;
#if defined(__riscv) && __riscv_xlen == 64
        // Inspect the width only after the first page is readable. A compressed
        // call at its end must not require the following page to be mapped.
        if (i == 0 && (*(volatile unsigned char *)(uintptr_t)gadget_addr & 3) != 3) {
            nop[0] = 0x01;
            patch_size = 2;
            page_count = 1 + (gadget_addr + patch_size - 1 - first_page) / page_size;
        }
#endif
    }

    // Byte stores also handle a full RV64 instruction at a halfword boundary.
    for (size_t i = 0; i < patch_size; i++)
        ((volatile unsigned char *)(uintptr_t)gadget_addr)[i] = nop[i];
    __builtin___clear_cache((char *)(uintptr_t)gadget_addr,
                            (char *)(uintptr_t)(gadget_addr + patch_size));

restore:
    for (size_t i = 0; i < changed_pages; i++) {
        if (mprotect((void *)(first_page + i * page_size), page_size, protections[i]) != 0) {
            report_error("Could not restore report page permissions\n");
            restore_failed = true;
        }
    }
    if (restore_failed)
        abort();
}

void report_gadget(const char * gadget_desc, int gadget_type, uint64_t gadget_addr, uint64_t access_addr, dift_tag_t tag) {
    if (!libcheckpoint_enabled)
        return;

    simulation_statistics.total_bug++;
    simulation_statistics.bug_type[gadget_type]++;

    report_text("[teapot], ");
    if (gadget_type < 0)
        report_character('-');
    report_number(gadget_type < 0 ? -(int64_t)gadget_type : gadget_type, 10);
    report_character(' ');
    report_text(gadget_desc);
    report_text(", 0x");
    report_number(gadget_addr, 16);
    report_text(", 0x");
    report_number(access_addr, 16);
    report_text(", 0x");
    report_number(tag, 16);
    report_text(", ");
    report_number(instruction_cnt, 10);
    report_text(", ");

    for (size_t i = checkpoint_cnt; i > 0; i--) {
        report_text("0x");
        report_number(checkpoint_metadata[i - 1].return_address, 16);
        report_text(", ");
    }
    report_character('\n');
    report_flush();

#ifdef SILENCE_GADGET_AFTER_FIRST_DISCOVERY
    make_report_call_nop(gadget_addr);
#endif
}

#if defined(__x86_64__)
/*
 * The x86-64 entry points are assembly wrappers that preserve the SIMD
 * register file before entering this C implementation.  A normal C call may
 * overwrite every XMM register, while report sites can occur in the middle of
 * hand-written assembly that keeps values live in those registers.
 */
#define DEF_REPORT_GADGET(TYPE) \
    void report_gadget_x64_impl_##TYPE(uint64_t gadget_addr, uint64_t access_addr, dift_tag_t tag) { \
        preserve_report_scratch_registers(); \
        report_gadget(STR(TYPE), GADGET_##TYPE, gadget_addr, access_addr, tag); \
        restore_report_scratch_registers(); \
    }

DEF_REPORT_GADGET(KASPER_CACHE);
DEF_REPORT_GADGET(KASPER_MDS);
DEF_REPORT_GADGET(KASPER_PORT);
#else
#define DEF_REPORT_GADGET(TYPE) \
    void report_gadget_##TYPE(uint64_t gadget_addr, uint64_t access_addr, dift_tag_t tag) { \
        report_gadget(STR(TYPE), GADGET_##TYPE, gadget_addr, access_addr, tag); \
    }

LIBCHECKPOINT_PRESERVE_MOST DEF_REPORT_GADGET(KASPER_CACHE);
LIBCHECKPOINT_PRESERVE_MOST DEF_REPORT_GADGET(KASPER_MDS);
LIBCHECKPOINT_PRESERVE_MOST DEF_REPORT_GADGET(KASPER_PORT);
#endif

#undef DEF_REPORT_GADGET
