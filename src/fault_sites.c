#define _GNU_SOURCE
#include "fault_sites.h"
#include "runtime_contract.h"
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>
#if FAULT_RISC_NATIVE
#include "fault_risc_template.h"
#endif

#if defined(ENABLE_FAULT_PUBLISHING) && defined(__riscv) && !defined(__riscv_compressed)
#error RISC-V fault publishing requires the C extension for halfword-aligned whole instructions
#endif

/* Startup must not prime the application's allocator or consume addresses
 * from mmap's placement cursor. Speculative reads of freed memory otherwise
 * find our /proc text, manufacturing gadgets even with adaptation disabled.
 * In particular, do not replace these raw reads with stdio/getline/sscanf. */
static long fault_syscall3(long number, long a, long b, long c) {
#if defined(__x86_64__)
    long result;
    __asm__ volatile ("syscall" : "=a"(result)
                      : "0"(number), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "cc", "memory");
    return result;
#elif defined(__aarch64__)
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "cc", "memory");
    return x0;
#elif defined(__riscv)
    register long a7 __asm__("a7") = number;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
    return a0;
#else
#error Unsupported fault runtime ISA
#endif
}

#if defined(ENABLE_FAULT_PUBLISHING) && FAULT_RISC_NATIVE
#include "fault_risc_publish.h"
#endif

#if FAULT_RISC_NATIVE
__attribute__((noreturn)) static void fault_raw_stop(void) {
    /* Neither a changed copy-fault context nor a partly restored executable
     * mapping may reach diagnostics, libc, or an application signal handler. */
    fault_syscall3(SYS_exit_group, 127, 0, 0);
    for (;;) __asm__ volatile ("" ::: "memory");
}
#endif

#if defined(__riscv) && __riscv_xlen == 64
static long fault_rv_syscall5(long number, long a, long b, long c, long d, long e) {
    register long a7 __asm__("a7") = number;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4) : "memory");
    return a0;
}
#endif

struct fault_risc_host_proof {
    uintptr_t low, high, page_size;
    uint32_t isa, low_failures;
    int64_t hwprobe_result, hwprobe_key, tagged_control;
    uint64_t hwprobe_value;
};
#if FAULT_RISC_NATIVE
static uintptr_t fault_risc_high_bound(struct fault_risc_host_proof *proof) {
    proof->isa = FAULT_RISC_NATIVE;
    proof->hwprobe_result = proof->tagged_control = -ENOSYS;
    proof->hwprobe_key = -1;
#if defined(__riscv) && __riscv_xlen == 64
    /* Linux UAPI: syscall 258, hwprobe key 7, PR_GET_TAGGED_ADDR_CTRL=56.
     * Query actual effective PMLEN, never satp or CPU capability inference.
     * Both queries run even when adaptation is disabled. Unknown => no high
     * predicate; a recognized hwprobe value is an exclusive user-space end.
     * See scripts/syscall.tbl and arch/riscv/kernel/{sys_hwprobe,process}.c. */
    struct { int64_t key; uint64_t value; } pair = {7, 0};
    long probe = fault_rv_syscall5(258, (long)&pair, 1, 0, 0, 0);
    long tagged = fault_rv_syscall5(SYS_prctl, 56, 0, 0, 0, 0);
    proof->hwprobe_result = probe; proof->hwprobe_key = pair.key;
    proof->hwprobe_value = pair.value; proof->tagged_control = tagged;
    const unsigned long pmlen_mask = 0x7fUL << 24;
    if (probe || pair.key != 7 || tagged < 0 ||
            ((unsigned long)tagged & ~(pmlen_mask | 1UL)) || ((unsigned long)tagged & pmlen_mask)) return 0;
    if (pair.value == (UINT64_C(1) << 38) || pair.value == (UINT64_C(1) << 47) ||
            pair.value == (UINT64_C(1) << 56)) return (uintptr_t)pair.value;
#endif
    /* A64 has no approved query proving its active VA ceiling. */
    return 0;
}
#endif

#define FAULT_MAP_LIMIT 8192
struct mapping { uintptr_t start, end; char permissions[5]; };
#if defined(__aarch64__)
#define FAULT_NOBITS "%nobits"
#define FAULT_OBJECT "%object"
#else
#define FAULT_NOBITS "@nobits"
#define FAULT_OBJECT "@object"
#endif
/* Zero-initialized private scratch needs no file bytes. Use explicit NOBITS,
 * as for the immutable pools below: a C section attribute alone produces
 * PROGBITS and grows the protected-data end beyond RVC LUI's reach. These
 * mutable objects precede the pools' whole-page alignment/protection. */
__asm__(".pushsection teapot_protected_bss,\"aw\"," FAULT_NOBITS "\n"
        ".balign 8\n.local startup_maps\n.type startup_maps," FAULT_OBJECT "\n"
        "startup_maps:\n.zero 196608\n.size startup_maps,.-startup_maps\n"
        ".balign 8\n.local proc_buffer\n.type proc_buffer," FAULT_OBJECT "\n"
        "proc_buffer:\n.zero 4096\n.size proc_buffer,.-proc_buffer\n.popsection\n");
/* The compiler must know that these assembly-local definitions are not
 * preemptible. Otherwise A64 PIE compilation emits GOT relocations which
 * the assembler reduces to one section symbol plus different addends; those
 * are not distinct GOT entries and alias the private objects at final link. */
extern struct mapping startup_maps[FAULT_MAP_LIMIT] __attribute__((aligned(8), visibility("hidden")));
extern unsigned char proc_buffer[4096] __attribute__((aligned(8), visibility("hidden")));
_Static_assert(sizeof(startup_maps) == 196608 && sizeof(proc_buffer) == 4096,
               "keep NOBITS startup scratch reservations in sync with their types");
LIBCHECKPOINT_ASSERT_PROTECTED(startup_maps);
LIBCHECKPOINT_ASSERT_PROTECTED(proc_buffer);

static long proc_open(const char *path) {
    return fault_syscall3(SYS_openat, AT_FDCWD, (long)path, O_RDONLY | O_CLOEXEC);
}
static long proc_read(long fd) {
    long result;
    do { result = fault_syscall3(SYS_read, fd, (long)proc_buffer, sizeof(proc_buffer)); }
    while (result == -EINTR);
    return result;
}
static bool proc_close(long fd) { return fault_syscall3(SYS_close, fd, 0, 0) == 0; }

static bool parse_hex(const char *line, size_t size, size_t *position, uintptr_t *value) {
    size_t begin = *position;
    uintptr_t result = 0;
    while (*position < size) {
        unsigned char ch = (unsigned char)line[*position];
        unsigned digit;
        if (ch >= '0' && ch <= '9') digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
        else break;
        if (result > (UINTPTR_MAX - digit) / 16) return false;
        result = result * 16 + digit;
        ++*position;
    }
    *value = result;
    return *position != begin;
}

static bool parse_mapping(const char *line, size_t size, struct mapping *m) {
    size_t p = 0;
    if (!parse_hex(line, size, &p, &m->start) || p == size || line[p++] != '-' ||
        !parse_hex(line, size, &p, &m->end) || m->start >= m->end || p == size || line[p++] != ' ')
        return false;
    while (p < size && line[p] == ' ') ++p;
    if (size - p < 5 || (line[p] != 'r' && line[p] != '-') ||
        (line[p+1] != 'w' && line[p+1] != '-') || (line[p+2] != 'x' && line[p+2] != '-') ||
        (line[p+3] != 'p' && line[p+3] != 's') || line[p+4] != ' ') return false;
    for (size_t i = 0; i < 4; ++i) m->permissions[i] = line[p+i];
    m->permissions[4] = 0;
    return true;
}

static bool read_mappings(size_t *count) {
    long fd = proc_open("/proc/self/maps");
    if (fd < 0) return false;
    /* Only the bounded header is needed; arbitrarily long pathnames do not
     * require storage or an allocation. Carry that header across short reads. */
    char prefix[64]; size_t used = 0, lines = 0; bool valid = true;
    long length;
    while ((length = proc_read(fd)) > 0 && valid) {
        for (long i = 0; i < length; ++i) {
            char ch = (char)proc_buffer[i];
            if (ch != '\n') {
                if (used < sizeof(prefix)) prefix[used++] = ch;
                continue;
            }
            if (lines == FAULT_MAP_LIMIT || !parse_mapping(prefix, used, &startup_maps[lines]) ||
                (lines && startup_maps[lines].start < startup_maps[lines - 1].end)) {
                valid = false; break;
            }
            ++lines; used = 0;
        }
    }
    valid = valid && length == 0 && used == 0 && lines != 0;
    if (!proc_close(fd)) valid = false;
    *count = lines;
    return valid;
}

static bool read_minimum(uint64_t *minimum) {
    long fd = proc_open("/proc/sys/vm/mmap_min_addr");
    if (fd < 0) return false;
    long length = proc_read(fd); bool valid = length > 0 && length < (long)sizeof(proc_buffer);
    uint64_t result = 0; size_t digits = 0;
    for (long i = 0; valid && i < length; ++i) {
        unsigned ch = proc_buffer[i];
        if (ch >= '0' && ch <= '9') {
            if ((size_t)i != digits || result > (UINT64_MAX - (ch - '0')) / 10) valid = false;
            else { result = result * 10 + ch - '0'; ++digits; }
        } else if (ch != '\n' && ch != ' ' && ch != '\t') valid = false;
    }
    if (!proc_close(fd)) valid = false;
    if (!valid || !digits) return false;
    *minimum = result;
    return true;
}

bool teapot_fault_resolve_relative(uintptr_t anchor, int64_t relative, uintptr_t *out) {
    uint64_t magnitude = relative < 0 ? 0 - (uint64_t)relative : (uint64_t)relative;
    if (relative < 0 ? magnitude > anchor : magnitude > UINTPTR_MAX - anchor)
        return false;
    *out = relative < 0 ? anchor - magnitude : anchor + magnitude;
    return true;
}

#define RESOLVE(t, field, out) teapot_fault_resolve_relative((uintptr_t)&(t)->field, (t)->field, &(out))

static const struct teapot_fault_site_entry *entry(const struct teapot_fault_site_table *t, size_t i) {
    return (const void *)((const unsigned char *)t + sizeof(*t) + i * t->entry_size);
}
static bool x64_windows(const struct teapot_fault_site_table *t) {
#if defined(__x86_64__)
    return t->version == TEAPOT_FAULT_WINDOW_VERSION && t->flags == TEAPOT_FAULT_X64_WINDOWS;
#else
    (void)t; return false;
#endif
}
static bool risc_windows(const struct teapot_fault_site_table *t) {
    return FAULT_RISC_NATIVE && t->version == TEAPOT_FAULT_RISC_VERSION && t->flags == TEAPOT_FAULT_RISC_WINDOWS;
}
static bool windows(const struct teapot_fault_site_table *t) {
    return x64_windows(t) || risc_windows(t);
}
uintptr_t teapot_fault_low_bound LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
LIBCHECKPOINT_ASSERT_PROTECTED(teapot_fault_low_bound);
#if FAULT_RISC_NATIVE
struct teapot_fault_risc_policy teapot_fault_risc_policy LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
LIBCHECKPOINT_ASSERT_PROTECTED(teapot_fault_risc_policy);
#endif
extern void restore_checkpoint_SIGSEGV(void) __attribute__((weak));

static bool validate_window(const struct teapot_fault_window_entry *e, uintptr_t pc, uintptr_t copy,
                            uintptr_t protected_start, uintptr_t protected_end) {
    uintptr_t ret, copy_end, bs, be, spill, low, rollback;
    if (!RESOLVE(e, return_pc, ret) || !RESOLVE(e, copy_end, copy_end) ||
        !RESOLVE(e, block_start, bs) || !RESOLVE(e, block_end, be) ||
        !RESOLVE(e, spill, spill) || !RESOLVE(e, low, low) || !RESOLVE(e, rollback, rollback) ||
        ret != pc + e->site.length || copy_end != copy + e->site.length ||
        bs > pc || be < ret || bs >= be || (spill & 7) || spill < protected_start ||
        protected_end - protected_start < 24 || spill > protected_end - 24 ||
        low != (uintptr_t)&teapot_fault_low_bound || !restore_checkpoint_SIGSEGV ||
        rollback != (uintptr_t)restore_checkpoint_SIGSEGV || e->padding ||
        e->access_length == 0 || e->access_length > e->site.length || e->access_length > 15 ||
        (e->origin != 1 && e->origin != 2) ||
        (e->rip_offset && (e->rip_offset < e->access_length || e->rip_offset + 4 > e->rip_end ||
                           e->rip_end > e->site.length)) || (!e->rip_offset && (e->rip_end || e->rip_target))) return false;
    for (size_t j = e->site.length; j < sizeof(e->original); ++j) if (e->original[j]) return false;
    for (size_t j = 0; j < sizeof(e->reserved); ++j) if (e->reserved[j]) return false;
    return true;
}

#if FAULT_RISC_NATIVE
static bool validate_risc_window(const struct teapot_fault_risc_entry *e, uintptr_t pc, uintptr_t stub,
        uintptr_t copy, uintptr_t text_start, uintptr_t text_end, uintptr_t stub_limit,
        uintptr_t protected_start, uintptr_t protected_end) {
    uintptr_t ret, copy_end, bs, be, spill, policy, rollback, stub_end;
    uint32_t replacement;
    unsigned char expected[128]; size_t copy_offset;
    if (!RESOLVE(e, return_pc, ret) || !RESOLVE(e, copy_end, copy_end) ||
        !RESOLVE(e, block_start, bs) || !RESOLVE(e, block_end, be) || !RESOLVE(e, spill, spill) ||
        !RESOLVE(e, policy, policy) || !RESOLVE(e, rollback, rollback) || !RESOLVE(e, stub_end, stub_end) ||
        ret != pc + 4 || copy_end != copy + 4 || bs < text_start || bs > pc || be < ret || be > text_end ||
        stub_end > stub_limit || stub_end <= copy_end || (spill & 7) || spill < protected_start ||
        protected_end - protected_start < e->spill_size || spill > protected_end - e->spill_size ||
        policy != (uintptr_t)&teapot_fault_risc_policy || !restore_checkpoint_SIGSEGV ||
        rollback != (uintptr_t)restore_checkpoint_SIGSEGV ||
        !fault_risc_branch(FAULT_RISC_NATIVE, pc, stub, &replacement)) return false;
    size_t size = fault_risc_template(e, FAULT_RISC_NATIVE, stub, spill, policy, ret, rollback,
                                      expected, sizeof(expected), &copy_offset);
    return size && stub_end - stub == size && copy == stub + copy_offset;
}
#endif

bool teapot_fault_validate_table(const struct teapot_fault_site_table *t, size_t bytes,
                                 uintptr_t protected_start, uintptr_t protected_end) {
    uintptr_t text_start, text_end, stub_start, stub_end, counters, counters_end, pending, pending_end;
    if (!t || ((uintptr_t)t & 7) || bytes < sizeof(*t) ||
        t->magic != TEAPOT_FAULT_SITE_MAGIC ||
        t->header_size != sizeof(*t) || t->threshold > 255 || !t->count ||
        (risc_windows(t) && t->count > UINT32_C(1048576)) ||
        t->reserved[0] || t->reserved[1] || t->reserved[2] ||
        !((t->version == TEAPOT_FAULT_SITE_VERSION && t->flags == TEAPOT_FAULT_SITE_TRAINING_ONLY &&
           t->entry_size == TEAPOT_FAULT_SITE_ENTRY_SIZE)
#ifdef ENABLE_FAULT_PUBLISHING
          || (x64_windows(t) && t->entry_size == TEAPOT_FAULT_WINDOW_ENTRY_SIZE)
          || (risc_windows(t) && t->entry_size == TEAPOT_FAULT_RISC_ENTRY_SIZE)
#endif
          ) || t->count > (bytes - sizeof(*t)) / t->entry_size)
        return false;
    if (!RESOLVE(t, text_start, text_start) || !RESOLVE(t, text_end, text_end) ||
        !RESOLVE(t, stub_start, stub_start) || !RESOLVE(t, stub_end, stub_end) ||
        !RESOLVE(t, counters_start, counters) || !RESOLVE(t, counters_end, counters_end) ||
        !RESOLVE(t, pending_start, pending) || !RESOLVE(t, pending_end, pending_end))
        return false;
    size_t storage = ((size_t)t->count + 7) & ~(size_t)7;
    if (text_start >= text_end || stub_start >= stub_end ||
        stub_start < text_start || stub_end > text_end || protected_start >= protected_end ||
        (counters & 7) || (pending & 7) || counters < protected_start || pending < protected_start ||
        counters_end < counters || pending_end < pending ||
        counters_end > protected_end || pending_end > protected_end ||
        counters_end - counters != storage || pending_end - pending != storage ||
        !(counters_end <= pending || pending_end <= counters))
        return false;
    /* Publishing may temporarily remove X from this range. The emitter and
     * final-link validator isolate it from ordinary/runtime .text pages. */
    if (windows(t) && ((text_start | text_end) & (risc_windows(t) ? TEAPOT_FAULT_RISC_ISOLATION - 1 : 4095)))
        return false;
    if (risc_windows(t) && (counters_end != pending || stub_start != text_start || stub_end != text_end)) return false;
    uintptr_t previous_end = 0, previous_spill_end = 0;
    for (size_t i = 0; i < t->count; ++i) {
        uintptr_t pc, stub, copy;
        const struct teapot_fault_site_entry *e = entry(t, i);
        unsigned length = e->length;
        if (!RESOLVE(e, fault_pc, pc) || !RESOLVE(e, stub, stub) ||
            !RESOLVE(e, copy_pc, copy) || e->flags || !length ||
            pc < text_start || pc >= text_end || length > text_end - pc ||
            stub < stub_start || stub >= stub_end ||
            copy < stub_start || copy >= stub_end || length > stub_end - copy ||
            (i && pc < previous_end))
            return false;
#if defined(__aarch64__)
        if (length != 4 || (pc & 3) || (stub & 3) || (copy & 3) ||
            (stub >= pc ? stub - pc >= (UINT64_C(1) << 27) :
                                     pc - stub > (UINT64_C(1) << 27))) return false;
#elif defined(__riscv)
        /* v4 requires C and permits a whole four-byte JAL at a halfword.
         * Preserve the historical v2 table contract; no alignment NOPs. */
        unsigned alignment_mask = risc_windows(t) ? 1 : 3;
        if (length != 4 || (pc & alignment_mask) || (stub & alignment_mask) || (copy & alignment_mask) ||
            (stub >= pc ? stub - pc >= (UINT64_C(1) << 20) :
                                     pc - stub > (UINT64_C(1) << 20))) return false;
#else
        if (length < 5 || length > (windows(t) ? TEAPOT_FAULT_MAX_WINDOW : 15) || pc > UINTPTR_MAX - 5 ||
            (stub >= pc + 5 ? stub - (pc + 5) > INT32_MAX :
                                    pc + 5 - stub > (UINT64_C(1) << 31))) return false;
#endif
        if (x64_windows(t)) {
            const struct teapot_fault_window_entry *w = (const void *)e;
            uintptr_t spill;
            if (!validate_window(w, pc, copy, protected_start, protected_end) ||
                !RESOLVE(w, spill, spill) || spill < previous_spill_end ||
                (spill < counters_end && counters < spill + 24) ||
                (spill < pending_end && pending < spill + 24)) return false;
            previous_spill_end = spill + 24;
#if FAULT_RISC_NATIVE
        } else if (risc_windows(t)) {
            const struct teapot_fault_risc_entry *w = (const void *)e;
            uintptr_t spill;
            if (!validate_risc_window(w, pc, stub, copy, text_start, text_end, stub_end,
                                      protected_start, protected_end) || !RESOLVE(w, spill, spill) ||
                spill != (i ? previous_spill_end : pending_end)) return false;
            previous_spill_end = spill + w->spill_size;
#endif
        }
        previous_end = pc + length;
    }
    return true;
}

bool teapot_fault_table_lookup(const struct teapot_fault_site_table *t, uintptr_t pc, size_t *site) {
    size_t lo = 0, hi = t->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uintptr_t target;
        if (!RESOLVE(entry(t, mid), fault_pc, target))
            return false;
        if (target < pc) lo = mid + 1; else hi = mid;
    }
    uintptr_t target;
    if (lo == t->count || !RESOLVE(entry(t, lo), fault_pc, target) || target != pc)
        return false;
    *site = lo;
    return true;
}

/* Byte-wide state, word-wide atomics: RV64 does not require byte AMOs. These
 * are relaxed because signal handlers and the safe point share one thread;
 * CAS also makes nested handler updates lossless. No allocation or locks. */
static uint8_t byte_update(uint32_t *words, size_t site, unsigned operation) {
    uint32_t *word = &words[site / 4];
    unsigned shift = (unsigned)(site % 4) * 8;
    uint32_t old = __atomic_load_n(word, __ATOMIC_RELAXED);
    for (;;) {
        unsigned byte = (old >> shift) & 255, next;
        if (operation == 0) {
            if (byte == 255) return 255;
            next = byte + 1;
        } else {
            if (byte != operation - 1) return 0;
            next = operation;
        }
        uint32_t value = (old & ~(UINT32_C(255) << shift)) | ((uint32_t)next << shift);
        if (__atomic_compare_exchange_n(word, &old, value, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return (uint8_t)next;
    }
}
uint8_t teapot_fault_increment(uint32_t *words, size_t site) { return byte_update(words, site, 0); }
bool teapot_fault_mark_pending(uint32_t *words, size_t site) { return byte_update(words, site, 1) == 1; }
bool teapot_fault_claim_pending(uint32_t *words, size_t site) { return byte_update(words, site, 2) == 2; }

struct teapot_fault_low_policy teapot_fault_low_policy(const struct teapot_fault_low_facts *f, uintptr_t bound) {
    unsigned failures = 0;
    if (!f || !f->known || !bound) failures |= TEAPOT_FAULT_LOW_UNKNOWN;
    if (!f || !f->enabled) failures |= TEAPOT_FAULT_LOW_DISABLED;
    if (f && f->known) {
        if (f->mmap_min_addr < bound) failures |= TEAPOT_FAULT_LOW_SYSCTL;
        if (f->cap_sys_rawio) failures |= TEAPOT_FAULT_LOW_CAPABILITY;
        if (f->mmap_page_zero) failures |= TEAPOT_FAULT_LOW_PERSONALITY;
        if (f->mapped_below_bound) failures |= TEAPOT_FAULT_LOW_MAPPING;
    }
    return (struct teapot_fault_low_policy){ failures ? 0 : bound, failures };
}

struct teapot_fault_low_policy teapot_fault_verify_low(uintptr_t bound, bool enabled) {
    struct teapot_fault_low_facts f = { .enabled = enabled };
    /* Collect the same facts on and off. Only the resulting permission differs;
     * no mode-dependent allocator or address-space history is allowed. */
    uint64_t minimum;
    if (!read_minimum(&minimum)) return teapot_fault_low_policy(&f, bound);
    f.mmap_min_addr = minimum;
    if (!bound) {
        long page = sysconf(_SC_PAGESIZE);
        if (page <= 0 || ((unsigned long)page & ((unsigned long)page - 1)) ||
            minimum > UINTPTR_MAX || minimum < (unsigned long)page)
            return teapot_fault_low_policy(&f, 0);
        /* Never round up: that would manufacture an unmappability guarantee. */
        bound = (uintptr_t)minimum & ~((uintptr_t)page - 1);
    }
    struct __user_cap_header_struct header = { .version = _LINUX_CAPABILITY_VERSION_3, .pid = 0 };
    struct __user_cap_data_struct caps[2] = {{0}};
    if (fault_syscall3(SYS_capget, (long)&header, (long)caps, 0) != 0)
        return teapot_fault_low_policy(&f, bound);
    /* Permitted as well as effective: a process can enable a permitted cap. */
    f.cap_sys_rawio = ((caps[CAP_SYS_RAWIO / 32].effective | caps[CAP_SYS_RAWIO / 32].permitted) &
                      (UINT32_C(1) << (CAP_SYS_RAWIO % 32))) != 0;
    long personality_flags = fault_syscall3(SYS_personality, 0xffffffffUL, 0, 0);
    if (personality_flags < 0) return teapot_fault_low_policy(&f, bound);
    f.mmap_page_zero = (personality_flags & MMAP_PAGE_ZERO) != 0;
    size_t count;
    f.known = read_mappings(&count);
    for (size_t i = 0; f.known && i < count; ++i)
        if (startup_maps[i].start < bound) f.mapped_below_bound = true;
    return teapot_fault_low_policy(&f, bound);
}

struct module_sites {
    uintptr_t start, end;
    const struct teapot_fault_site_table *table;
    const uint32_t *copies; /* Original entry indexes, sorted by copied PC. */
    uint32_t *counters, *pending;
};
/* A bounded NOBITS pool, not anonymous mmap and not the application heap.
 * Each immutable pool has whole pages, independently protected after init.
 * A million sites need only 4 MiB (indexes, not repeated pointers). Capacity
 * exhaustion fails before a partial registry can enter simulation. */
#define FAULT_MODULE_LIMIT 256
#define FAULT_COPY_LIMIT 1048576
#ifdef ENABLE_FAULT_TRAINING
union registry_pool {
    struct {
        struct module_sites modules[FAULT_MODULE_LIMIT];
        struct fault_risc_host_proof proof;
    } state;
    unsigned char pages[65536];
};
__asm__(".pushsection teapot_protected_bss,\"aw\"," FAULT_NOBITS "\n"
        ".balign 65536\n.local fault_registry_pool\n.type fault_registry_pool," FAULT_OBJECT "\n"
        "fault_registry_pool:\n.zero 65536\n.size fault_registry_pool,.-fault_registry_pool\n"
        ".balign 65536\n.local fault_copy_pool\n.type fault_copy_pool," FAULT_OBJECT "\n"
        "fault_copy_pool:\n.zero 4194304\n.size fault_copy_pool,.-fault_copy_pool\n.popsection\n");
extern union registry_pool fault_registry_pool __attribute__((aligned(65536), visibility("hidden")));
extern uint32_t fault_copy_pool[FAULT_COPY_LIMIT] __attribute__((aligned(65536), visibility("hidden")));
_Static_assert(sizeof(fault_registry_pool) == 65536 && sizeof(fault_copy_pool) == 4194304,
               "keep immutable registry pools on exclusive whole pages");
LIBCHECKPOINT_ASSERT_PROTECTED(fault_registry_pool);
LIBCHECKPOINT_ASSERT_PROTECTED(fault_copy_pool);
#endif
/* Mutable pointers to immutable mappings are runtime state, not application
 * state. These globals and emitted counters must never enter rollback logs. */
static const struct module_sites *registry LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
static size_t registry_count LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
static const void *registry_records LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
static struct teapot_fault_low_policy low_policy LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
static uint64_t training_enabled LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
#ifdef ENABLE_FAULT_PUBLISHING
/* The bounded queue serves the publisher only. Training-only runtimes keep
 * their reviewed pending-byte API and need no extra protected ring storage. */
#define EVENT_RING_SIZE 256
struct ring_event { size_t module, site; uint32_t ready; };
static struct ring_event event_ring[EVENT_RING_SIZE] LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
static struct { uint32_t count, pending, overflow, padding; } ring_state LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
#define event_count ring_state.count
#define events_pending ring_state.pending
#define event_overflow ring_state.overflow
static uint64_t publication_disabled LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
static uintptr_t publication_page_size LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
LIBCHECKPOINT_ASSERT_PROTECTED(event_ring);
LIBCHECKPOINT_ASSERT_PROTECTED(ring_state);
LIBCHECKPOINT_ASSERT_PROTECTED(publication_disabled);
LIBCHECKPOINT_ASSERT_PROTECTED(publication_page_size);
#endif
LIBCHECKPOINT_ASSERT_PROTECTED(registry);
LIBCHECKPOINT_ASSERT_PROTECTED(registry_count);
LIBCHECKPOINT_ASSERT_PROTECTED(registry_records);
LIBCHECKPOINT_ASSERT_PROTECTED(low_policy);
LIBCHECKPOINT_ASSERT_PROTECTED(training_enabled);
extern const char __start_teapot_protected_bss[] __attribute__((weak));
extern const char __stop_teapot_protected_bss[] __attribute__((weak));

static bool mapped(const struct mapping *maps, size_t count, uintptr_t start, uintptr_t end,
                   bool writable, bool executable) {
    if (start >= end) return false;
    for (size_t i = 0; i < count && start < end; ++i) {
        if (maps[i].end <= start) continue;
        if (maps[i].start > start || maps[i].permissions[0] != 'r' ||
            (maps[i].permissions[1] == 'w') != writable ||
            (maps[i].permissions[2] == 'x') != executable)
            return false;
        start = maps[i].end < end ? maps[i].end : end;
    }
    return start == end;
}
__attribute__((noreturn)) static void reject_sites(const char *reason) {
    fprintf(stderr, "libcheckpoint: invalid fault-site metadata: %s\n", reason);
    abort();
}
#ifdef ENABLE_FAULT_TRAINING
static bool registry_storage_overlap(uintptr_t start, uintptr_t end) {
    uintptr_t modules = (uintptr_t)&fault_registry_pool, copies = (uintptr_t)fault_copy_pool;
    return (start < modules + sizeof(fault_registry_pool) && modules < end) ||
           (start < copies + sizeof(fault_copy_pool) && copies < end);
}
#if FAULT_RISC_NATIVE
static bool risc_private_overlap(uintptr_t start, uintptr_t end) {
    uintptr_t maps = (uintptr_t)startup_maps, buffer = (uintptr_t)proc_buffer;
    uintptr_t policy = (uintptr_t)&teapot_fault_risc_policy;
    return registry_storage_overlap(start, end) ||
           (start < maps + sizeof(startup_maps) && maps < end) ||
           (start < buffer + sizeof(proc_buffer) && buffer < end) ||
           (start < policy + sizeof(teapot_fault_risc_policy) && policy < end);
}
#endif
static void sort_modules(struct module_sites *modules, size_t count) {
    /* At most 256 modules: insertion sort needs no heap or scratch mapping. */
    for (size_t i = 1; i < count; ++i) {
        struct module_sites saved = modules[i];
        size_t position = i;
        while (position && modules[position - 1].start > saved.start) {
            modules[position] = modules[position - 1]; --position;
        }
        modules[position] = saved;
    }
}

static uintptr_t copy_address(const struct teapot_fault_site_table *t, uint32_t index) {
    uintptr_t pc;
    if (index >= t->count || !RESOLVE(entry(t, index), copy_pc, pc))
        reject_sites("invalid copied access index");
    return pc;
}
static void sift_copies(const struct teapot_fault_site_table *t, uint32_t *indexes,
                        size_t count, size_t root) {
    uint32_t saved = indexes[root];
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && copy_address(t, indexes[child]) < copy_address(t, indexes[child + 1]))
            ++child;
        if (copy_address(t, saved) >= copy_address(t, indexes[child])) break;
        indexes[root] = indexes[child]; root = child;
    }
    indexes[root] = saved;
}
static void sort_copies(const struct teapot_fault_site_table *t, uint32_t *indexes) {
    /* qsort is not allocation-free on glibc. In-place heapsort is bounded
     * O(n log n) even for large, adversarially ordered module tables. */
    for (size_t i = t->count / 2; i; --i) sift_copies(t, indexes, t->count, i - 1);
    for (size_t i = t->count; i > 1; --i) {
        uint32_t swap = indexes[0]; indexes[0] = indexes[i - 1]; indexes[i - 1] = swap;
        sift_copies(t, indexes, i - 1, 0);
    }
}
#endif

static bool original_overlap(const struct teapot_fault_site_table *t, uintptr_t pc, size_t length) {
    size_t lo = 0, hi = t->count;
    /* First original instruction whose end is after this copy's start. */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2; uintptr_t original;
        RESOLVE(entry(t, mid), fault_pc, original);
        if (original + entry(t, mid)->length <= pc) lo = mid + 1; else hi = mid;
    }
    uintptr_t original;
    return lo < t->count && RESOLVE(entry(t, lo), fault_pc, original) && original < pc + length;
}

#ifdef ENABLE_FAULT_TRAINING
static const uint32_t *initialize_copies(const struct teapot_fault_site_table *t, uint32_t *copies,
                                        const struct mapping *maps, size_t nmap) {
    for (size_t i = 0; i < t->count; ++i) {
        uintptr_t original, copy;
        const struct teapot_fault_site_entry *e = entry(t, i);
        RESOLVE(e, fault_pc, original); RESOLVE(e, copy_pc, copy);
        /* Mapping and extent checks precede these reads. PC-relative accesses
         * are unsupported by the emitter/final validator: copies are exact. */
        if (original_overlap(t, copy, e->length))
            reject_sites("copy differs from or overlaps an original access");
        if (x64_windows(t)) {
#ifdef ENABLE_FAULT_PUBLISHING
            const struct teapot_fault_window_entry *w = (const void *)e;
            uintptr_t stub, se, spill;
            RESOLVE(e, stub, stub); RESOLVE(t, stub_end, se); RESOLVE(w, spill, spill);
            if (registry_storage_overlap(spill, spill + 24) || !mapped(maps, nmap, spill, spill + 24, true, false))
                reject_sites("spill mapping or immutable registry overlap");
            unsigned char template[160], original_window[24];
            size_t size = teapot_fault_x64_template(w, template, sizeof(template));
            if (!size || size > se - stub ||
                !teapot_fault_x64_window(w,original,original_window,sizeof(original_window)) ||
                memcmp((const void *)original, original_window, e->length) ||
                memcmp((const void *)stub, template, size) ||
                ((const uint64_t *)spill)[0] || ((const uint64_t *)spill)[1] || ((const uint64_t *)spill)[2])
                reject_sites("window/stub template or private spill differs");
#else
            reject_sites("publisher metadata in a training-only runtime");
#endif
        } else if (risc_windows(t)) {
#if defined(ENABLE_FAULT_PUBLISHING) && FAULT_RISC_NATIVE
            const struct teapot_fault_risc_entry *w = (const void *)e;
            uintptr_t stub, spill, policy, ret, rollback;
            RESOLVE(e, stub, stub); RESOLVE(w, spill, spill); RESOLVE(w, policy, policy);
            RESOLVE(w, return_pc, ret); RESOLVE(w, rollback, rollback);
            if (risc_private_overlap(spill, spill + w->spill_size) ||
                !mapped(maps, nmap, spill, spill + w->spill_size, true, false) ||
                !mapped(maps, nmap, policy, policy + sizeof(teapot_fault_risc_policy), true, false))
                reject_sites("RISC spill/policy mapping or runtime-private overlap");
            unsigned char expected[128]; size_t copy_offset;
            size_t size = fault_risc_template(w, FAULT_RISC_NATIVE, stub, spill, policy, ret, rollback,
                                              expected, sizeof(expected), &copy_offset);
            if (!size || original_overlap(t, stub, size) || memcmp((const void *)stub, expected, size))
                reject_sites("RISC stub template differs or overlaps an original");
            for (unsigned j = 0; j < 4; ++j)
                if (((const unsigned char *)original)[j] != (unsigned char)(w->original >> (8 * j)))
                    reject_sites("RISC original instruction differs");
            for (size_t j = 0; j < w->spill_size / 8; ++j)
                if (((const uint64_t *)spill)[j]) reject_sites("RISC spill is not initially zero");
#else
            reject_sites("RISC publisher metadata in a training-only runtime");
#endif
        } else if (memcmp((const void *)original, (const void *)copy, e->length))
            reject_sites("copy differs from original access");
#if defined(__riscv)
        if ((*(const unsigned char *)original & 3) != 3)
            reject_sites("compressed RISC-V access is not patchable");
#endif
        copies[i] = (uint32_t)i;
    }
    sort_copies(t, copies);
    for (size_t i = 1; i < t->count; ++i) {
        if (copy_address(t, copies[i - 1]) + entry(t, copies[i - 1])->length > copy_address(t, copies[i]))
            reject_sites("overlapping or duplicate copied access PCs");
        if (risc_windows(t)) {
            const struct teapot_fault_risc_entry *previous = (const void *)entry(t, copies[i - 1]);
            const struct teapot_fault_risc_entry *next = (const void *)entry(t, copies[i]);
            uintptr_t previous_end, next_start;
            RESOLVE(previous, stub_end, previous_end); RESOLVE(&next->site, stub, next_start);
            if (previous_end > next_start) reject_sites("overlapping RISC stubs");
        }
    }
    return copies;
}
#endif

/* Records have already passed the contract walk. This allocation-free pass
 * performs raw file I/O, bounds and mapping checks before any simulation. */
void teapot_fault_registry_initialize(const void *begin, const void *end) {
    extern char **environ;
    teapot_fault_registry_initialize_environment(begin,end,environ);
}

void teapot_fault_registry_initialize_environment(const void *begin, const void *end,
                                                 char *const *environment) {
    if (registry_records) {
        if (registry_records != begin) reject_sites("module registry changed after initialization");
        return;
    }
    size_t count = 0;
    for (const char *p = begin; p < (const char *)end;) {
        if (*(const uint64_t *)p == 0) { p += 8; continue; }
        const struct libcheckpoint_contract_record *r = (const void *)p;
        bool required = (r->capabilities & LIBCHECKPOINT_CAPABILITY_FAULT_TRAINING) != 0;
        if (required != (r->fault_sites != NULL)) reject_sites("table/capability disagreement");
        if ((r->capabilities & LIBCHECKPOINT_CAPABILITY_FAULT_PUBLISHING) && !required)
            reject_sites("publisher without a training table");
        if (required) ++count;
        p += (r->header_size + (size_t)r->json_size + 7) & ~(size_t)7;
    }
    if (!count) { registry_records = begin; return; }
#ifndef ENABLE_FAULT_TRAINING
    reject_sites("this runtime has no fault training capability");
#else
    if (count > FAULT_MODULE_LIMIT) reject_sites("fault module capacity exceeded");
    long host_page = sysconf(_SC_PAGESIZE);
#if FAULT_RISC_NATIVE
    if (host_page != 4096 && host_page != 16384 && host_page != 65536) {
        /* Unproved host page geometry: never publish or consume the table.
         * Original accesses remain on the kernel path in both toggle modes. */
        low_policy.failures = TEAPOT_FAULT_LOW_UNKNOWN | TEAPOT_FAULT_LOW_DISABLED;
        registry_records = begin;
        return;
    }
#endif
    if (host_page <= 0 || ((unsigned long)host_page & ((unsigned long)host_page - 1)) || host_page > 65536)
        reject_sites("unsupported registry page size");
    size_t nmap;
    if (!read_mappings(&nmap)) reject_sites("cannot verify mapping permissions within bounded snapshot");
    const struct mapping *maps = startup_maps;
    struct module_sites *snapshot = fault_registry_pool.state.modules;
    size_t copies_used = 0;
    size_t index = 0;
    for (const char *p = begin; p < (const char *)end;) {
        if (*(const uint64_t *)p == 0) { p += 8; continue; }
        const struct libcheckpoint_contract_record *r = (const void *)p;
        const struct teapot_fault_site_table *t = r->fault_sites;
        p += (r->header_size + (size_t)r->json_size + 7) & ~(size_t)7;
        if (!t) continue;
        uintptr_t address = (uintptr_t)t;
        if (address & 7 || address > UINTPTR_MAX - sizeof(*t) ||
            !mapped(maps, nmap, address, address + sizeof(*t), false, false))
            reject_sites("table header is not read-only non-executable memory");
        if ((r->capabilities & LIBCHECKPOINT_CAPABILITY_FAULT_PUBLISHING) !=
            (windows(t) ? LIBCHECKPOINT_CAPABILITY_FAULT_PUBLISHING : 0))
            reject_sites("publisher capability/format disagreement");
#ifdef ENABLE_FAULT_PUBLISHING
        if (windows(t) && !publication_page_size) {
            long page = host_page;
            /* The x64 emitter/final ELF contract isolates 4-KiB base pages.
             * Query before instrumentation, never through libc while RW. */
            if (x64_windows(t) ? page != 4096 : (page != 4096 && page != 16384 && page != 65536))
                reject_sites("unsupported publisher base-page size");
            publication_page_size = (uintptr_t)page;
        }
#endif
        if (!t->entry_size || t->count > (SIZE_MAX - sizeof(*t)) / t->entry_size)
            reject_sites("table size overflow");
        size_t extent = sizeof(*t) + (size_t)t->count * t->entry_size;
        if (address > UINTPTR_MAX - extent ||
            !mapped(maps, nmap, address, address + extent, false, false) ||
            !teapot_fault_validate_table(t, extent, (uintptr_t)__start_teapot_protected_bss,
                                        (uintptr_t)__stop_teapot_protected_bss))
            reject_sites("malformed table, relative address, bounds, or order");
        uintptr_t cs, ce, ps, pe, ss, se;
        struct module_sites *m = &snapshot[index++];
        RESOLVE(t, text_start, m->start); RESOLVE(t, text_end, m->end);
        RESOLVE(t, stub_start, ss); RESOLVE(t, stub_end, se);
        RESOLVE(t, counters_start, cs); RESOLVE(t, counters_end, ce);
        RESOLVE(t, pending_start, ps); RESOLVE(t, pending_end, pe);
        if (registry_storage_overlap(cs, ce) || registry_storage_overlap(ps, pe))
            reject_sites("counter aliases immutable registry storage");
#if FAULT_RISC_NATIVE
        if (risc_windows(t) && (risc_private_overlap(cs, ce) || risc_private_overlap(ps, pe)))
            reject_sites("RISC counter aliases runtime-private storage");
#endif
        if (!mapped(maps, nmap, m->start, m->end, false, true) ||
            !mapped(maps, nmap, ss, se, false, true) ||
            !mapped(maps, nmap, cs, ce, true, false) || !mapped(maps, nmap, ps, pe, true, false))
            reject_sites("text/counter mapping permissions");
        m->table = t; m->counters = (void *)cs; m->pending = (void *)ps;
        if (t->count > FAULT_COPY_LIMIT - copies_used) reject_sites("fault site capacity exceeded");
        m->copies = initialize_copies(t, fault_copy_pool + copies_used, maps, nmap);
        copies_used += t->count;
        for (size_t j = 0; j < (ce - cs) / 4; ++j)
            if (m->counters[j] || m->pending[j]) reject_sites("counter area is not initially zero");
        for (size_t j = 0; j + 1 < index; ++j) {
            uintptr_t other_cs, other_ce, other_ps, other_pe;
            RESOLVE(snapshot[j].table, counters_start, other_cs); RESOLVE(snapshot[j].table, counters_end, other_ce);
            RESOLVE(snapshot[j].table, pending_start, other_ps); RESOLVE(snapshot[j].table, pending_end, other_pe);
            if ((cs < other_ce && other_cs < ce) || (ps < other_pe && other_ps < pe) ||
                (cs < other_pe && other_ps < ce) || (ps < other_ce && other_cs < pe))
                reject_sites("modules share counter storage");
            /* v4 state is one contiguous counter/pending/spill reservation,
             * checked above. Compare whole owned ranges, not only counters. */
            if (risc_windows(t) || risc_windows(snapshot[j].table)) {
                uintptr_t this_end = pe, other_end = other_pe;
                if (risc_windows(t)) {
                    const struct teapot_fault_risc_entry *last = (const void *)entry(t, t->count - 1);
                    RESOLVE(last, spill, this_end); this_end += last->spill_size;
                }
                if (risc_windows(snapshot[j].table)) {
                    const struct teapot_fault_site_table *other = snapshot[j].table;
                    const struct teapot_fault_risc_entry *last = (const void *)entry(other, other->count - 1);
                    RESOLVE(last, spill, other_end); other_end += last->spill_size;
                }
                uintptr_t this_start = cs < ps ? cs : ps, other_start = other_cs < other_ps ? other_cs : other_ps;
                if (ce > this_end) this_end = ce;
                if (other_ce > other_end) other_end = other_ce;
                if (this_start < other_end && other_start < this_end) reject_sites("modules share RISC state");
            }
        }
    }
    sort_modules(snapshot, count);
    for (size_t i = 1; i < count; ++i)
        if (snapshot[i - 1].end > snapshot[i].start) reject_sites("overlapping module text ranges");
#if FAULT_RISC_NATIVE
    /* Keep query results in the immutable pool, apart from mutable guard
     * enables. The same collector runs with adaptation on and off. It uses
     * only startup scratch, no heap or anonymous mapping placement history. */
    struct teapot_fault_low_policy verified_low = teapot_fault_verify_low(0, true);
    struct fault_risc_host_proof *proof = &fault_registry_pool.state.proof;
    proof->low = verified_low.bound; proof->low_failures = verified_low.failures;
    proof->page_size = (uintptr_t)host_page;
    proof->high = fault_risc_high_bound(proof);
#endif
    if (fault_syscall3(SYS_mprotect, (long)&fault_registry_pool, sizeof(fault_registry_pool), PROT_READ) ||
        fault_syscall3(SYS_mprotect, (long)fault_copy_pool, sizeof(fault_copy_pool), PROT_READ))
        reject_sites("registry protection");
#ifdef ENABLE_FAULT_TRAINING
    const char *setting = NULL;
    const char prefix[] = "TEAPOT_FAULT_ADAPTATION=";
    if (environment) {
        for (char *const *p=environment;*p;p++) {
            if (!strncmp(*p,prefix,sizeof(prefix)-1)) {
                setting=*p+sizeof(prefix)-1;
                break;
            }
        }
    }
    /* A missing environment is uncertainty, not permission to ignore '0'. */
    training_enabled = environment && (!setting || strcmp(setting, "1") == 0);
#ifdef ENABLE_FAULT_PUBLISHING
    bool publisher=false;
    for (size_t i=0;i<count;i++) publisher |= windows(snapshot[i].table);
#if defined(__x86_64__)
    if (publisher && !teapot_fault_x64_can_publish_addresses())
        training_enabled=0;
#elif defined(__riscv) && __riscv_xlen == 64
    /* Flags 0 synchronizes the process across migration. A denied/missing
     * syscall leaves the kernel path; local fence.i is not a substitute.
     * Run this preflight identically with the environment toggle on and off. */
    if (publisher && fault_syscall3(259, 0, 0, 0)) training_enabled=0;
#else
    (void)publisher;
#endif
#endif
#else
    training_enabled = 0;
#endif
#if FAULT_RISC_NATIVE
    low_policy = verified_low;
    if (!training_enabled) { low_policy.bound = 0; low_policy.failures |= TEAPOT_FAULT_LOW_DISABLED; }
    teapot_fault_risc_policy.low = low_policy.bound;
    teapot_fault_risc_policy.high = training_enabled ? proof->high : 0;
#else
    low_policy = teapot_fault_verify_low(0, training_enabled != 0);
#endif
    teapot_fault_low_bound = low_policy.bound;
    registry = snapshot; registry_count = count; registry_records = begin;
#endif
}

bool teapot_fault_lookup(uintptr_t pc, size_t *module, size_t *site) {
    size_t lo = 0, hi = registry_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (registry[mid].end <= pc) lo = mid + 1; else hi = mid;
    }
    if (lo == registry_count || pc < registry[lo].start) return false;
    const struct module_sites *m = &registry[lo];
    if (!teapot_fault_table_lookup(m->table, pc, site)) {
        /* Copies need not have the same order as originals. Never linear scan
         * or re-interpret an interior PC as a new site in the signal handler. */
        size_t left = 0, right = m->table->count;
        while (left < right) {
            size_t mid = left + (right - left) / 2;
            uintptr_t copied_pc;
            if (!RESOLVE(entry(m->table, m->copies[mid]), copy_pc, copied_pc)) return false;
            if (copied_pc < pc) left = mid + 1; else right = mid;
        }
        uintptr_t copied_pc;
        if (left == m->table->count || !RESOLVE(entry(m->table, m->copies[left]), copy_pc, copied_pc) || copied_pc != pc)
            return false;
        *site = m->copies[left];
    }
    *module = lo;
    return true;
}
#if FAULT_RISC_NATIVE
bool teapot_fault_risc_copied_kernel(uintptr_t pc, bool active, bool replay) {
    size_t module, site;
    if (!teapot_fault_lookup(pc, &module, &site) || !risc_windows(registry[module].table))
        return false;
    uintptr_t copy;
    if (!RESOLVE(entry(registry[module].table, site), copy_pc, copy) || copy != pc) return false;
    if (!active || replay) fault_raw_stop();
    return true;
}
#endif
bool teapot_fault_train(int sig, const siginfo_t *info, uintptr_t pc, bool active, bool replay) {
    /* MAPERR/ACCERR are synchronous data faults at an emitted access PC.
     * SI_KERNEL (e.g. x64 #GP), MTE, BTI, PAC and asynchronous signals are not
     * inferred from si_addr. A backend must supply proven EA facts for #GP. */
    if (!training_enabled || sig != SIGSEGV || !info ||
        (info->si_code != SEGV_MAPERR && info->si_code != SEGV_ACCERR && info->si_code != SI_KERNEL) || !active || replay)
        return false;
    size_t module, site;
    if (!teapot_fault_lookup(pc, &module, &site)) return false;
    const struct module_sites *m = &registry[module];
    if (info->si_code == SI_KERNEL && !windows(m->table)) return false;
    if (!m->table->threshold) return false;
    uint8_t value = teapot_fault_increment(m->counters, site);
    if (value >= m->table->threshold && teapot_fault_mark_pending(m->pending, site)) {
#ifdef ENABLE_FAULT_PUBLISHING
        uint32_t index = __atomic_load_n(&event_count, __ATOMIC_RELAXED);
        while (index < EVENT_RING_SIZE &&
               !__atomic_compare_exchange_n(&event_count, &index, index + 1, false,
                                             __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        if (index < EVENT_RING_SIZE) {
            event_ring[index].module = module; event_ring[index].site = site;
            __atomic_store_n(&event_ring[index].ready, 1, __ATOMIC_RELEASE);
        } else __atomic_store_n(&event_overflow, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&events_pending, 1, __ATOMIC_RELEASE);
#endif
    }
    return true;
}
uint8_t teapot_fault_counter(size_t module, size_t site) {
    if (module >= registry_count || site >= registry[module].table->count) return 0;
    uint32_t value = __atomic_load_n(&registry[module].counters[site / 4], __ATOMIC_RELAXED);
    return (uint8_t)(value >> ((site % 4) * 8));
}
bool teapot_fault_next_pending(struct teapot_fault_event *event) {
    if (!event) return false;
    for (size_t i = 0; i < registry_count; ++i) {
        const struct module_sites *m = &registry[i];
        for (size_t j = 0; j < m->table->count; ++j) {
            if (!teapot_fault_claim_pending(m->pending, j)) continue;
            event->module = i; event->site = j; event->count = teapot_fault_counter(i, j);
            RESOLVE(entry(m->table, j), fault_pc, event->fault_pc);
            RESOLVE(entry(m->table, j), stub, event->stub);
            RESOLVE(entry(m->table, j), copy_pc, event->copy_pc);
            event->length = entry(m->table, j)->length;
            return true;
        }
    }
    return false;
}
struct teapot_fault_low_policy teapot_fault_current_low_policy(void) { return low_policy; }

#ifdef ENABLE_FAULT_PUBLISHING
/* The whole process is single-threaded. In particular an application signal
 * callback must not re-enter the rewriter/runtime's global scratch storage.
 * All signals are blocked during publication; the signal handler only trains.
 * Replace no bytes until the complete window is writable. On an RX-restore
 * failure undo the JMP while still writable, then restore RX or stop safely.
 * Resuming on a non-executable page cannot preserve the kernel fallback. */
static bool publish_site(size_t module, size_t site) {
    if (module >= registry_count || site >= registry[module].table->count) reject_sites("bad queued site");
    const struct module_sites *m = &registry[module];
    if (!windows(m->table)) return true; /* training-only tables never publish */
#if FAULT_RISC_NATIVE
    if (!risc_windows(m->table)) return false;
    const struct teapot_fault_risc_entry *w = (const void *)entry(m->table, site);
    uintptr_t pc, stub;
    uint32_t replacement;
    if (!RESOLVE(&w->site, fault_pc, pc) || !RESOLVE(&w->site, stub, stub) ||
        !fault_risc_branch(FAULT_RISC_NATIVE, pc, stub, &replacement)) return false;
    uintptr_t page = publication_page_size;
    if (!page || pc > UINTPTR_MAX - 4) return false;
    uintptr_t first = pc & ~(page - 1);
    size_t count = 1 + ((pc + 3) / page - pc / page);
    struct fault_risc_page pages[2];
    for (size_t i = 0; i < count; ++i) {
        pages[i].address = first + i * page;
        if (pages[i].address < m->start || pages[i].address > m->end - page ||
            !fault_risc_current_page_policy(pages[i].address, page, PROT_READ | PROT_EXEC,
                                            &pages[i].original_protection)) return false;
    }
    enum fault_risc_patch_result result = fault_risc_publish_word(pc, w->original, replacement, page, pages, count);
    if (result == FAULT_RISC_PATCH_UNSAFE) fault_raw_stop();
    return result == FAULT_RISC_PATCH_PUBLISHED;
#else
    const struct teapot_fault_window_entry *w = (const void *)entry(m->table, site);
    uintptr_t pc, stub;
    RESOLVE(&w->site, fault_pc, pc); RESOLVE(&w->site, stub, stub);
    unsigned char original[24];
    if (!teapot_fault_x64_window(w,pc,original,sizeof(original))) reject_sites("invalid patch window");
    for (size_t i=0;i<w->site.length;i++)
        if (((const volatile unsigned char *)pc)[i]!=original[i])
            reject_sites("patch window changed since startup");
    uintptr_t page = publication_page_size;
    if (page != 4096) return false;
    uintptr_t first = pc & ~(page - 1);
    uintptr_t last = (pc + 5 + page - 1) & ~(page - 1);
    if (last < pc || teapot_fault_x64_mprotect((void *)first, last - first, PROT_READ | PROT_WRITE)) return false;
    unsigned char branch[5] = {0xe9};
    int32_t displacement = stub >= pc + 5 ? (int32_t)(stub - pc - 5) :
                           (int32_t)(0u - (uint32_t)(pc + 5 - stub));
    for (size_t i=0;i<4;i++) ((volatile unsigned char *)branch)[i+1]=((const unsigned char *)&displacement)[i];
    /* No other thread or signal may fetch a partly written instruction. x64
     * same-thread SMC is coherent; only a later restored path enters this PC. */
    for (size_t i=0;i<sizeof(branch);i++) ((volatile unsigned char *)pc)[i]=branch[i];
    __asm__ volatile ("" ::: "memory");
    if (!teapot_fault_x64_mprotect((void *)first, last - first, PROT_READ | PROT_EXEC)) return true;
    for (size_t i=0;i<sizeof(branch);i++) ((volatile unsigned char *)pc)[i]=original[i];
    __asm__ volatile ("" ::: "memory");
    if (teapot_fault_x64_mprotect((void *)first, last - first, PROT_READ | PROT_EXEC))
        reject_sites("cannot restore executable protections after undoing publication");
    return false;
#endif
}
#endif

void teapot_fault_publish_pending(void) {
#ifdef ENABLE_FAULT_PUBLISHING
    if (publication_disabled || !__atomic_load_n(&events_pending, __ATOMIC_ACQUIRE)) return;
    /* Linux's kernel signal mask is 64 bits on the supported ISAs. The raw
     * syscall avoids libc/ASan writing a 128-byte sigset_t on the poisoned
     * private stack; the kernel itself ignores unmaskable SIGKILL/SIGSTOP. */
    uint64_t all=UINT64_MAX,previous=0;
    if (syscall(SYS_rt_sigprocmask,SIG_BLOCK,&all,&previous,sizeof(all))) {
        publication_disabled = 1; return;
    }
    uint32_t count = __atomic_load_n(&event_count, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < count && !publication_disabled; ++i) {
        if (!__atomic_load_n(&event_ring[i].ready, __ATOMIC_ACQUIRE)) reject_sites("incomplete event ring");
        size_t module = event_ring[i].module, site = event_ring[i].site;
        if (!teapot_fault_claim_pending(registry[module].pending, site)) continue;
        if (!publish_site(module, site)) publication_disabled = 1;
    }
    if (__atomic_load_n(&event_overflow, __ATOMIC_RELAXED) && !publication_disabled) {
        struct teapot_fault_event event;
        while (teapot_fault_next_pending(&event))
            if (!publish_site(event.module, event.site)) { publication_disabled = 1; break; }
    }
    for (uint32_t i = 0; i < count; ++i) event_ring[i].ready = 0;
    __atomic_store_n(&event_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&event_overflow, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&events_pending, 0, __ATOMIC_RELEASE);
    if (publication_disabled) {
        training_enabled = 0; teapot_fault_low_bound = 0;
#if FAULT_RISC_NATIVE
        teapot_fault_risc_policy.low = teapot_fault_risc_policy.high = 0;
#endif
    }
    if (syscall(SYS_rt_sigprocmask,SIG_SETMASK,&previous,NULL,sizeof(previous)))
        reject_sites("cannot restore signal mask");
#endif
}
