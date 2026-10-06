#pragma once

/* Training v2 never modifies text. The separately validated x64 publisher
 * uses v3 windows and writes only at the post-rollback safe point. Each
 * signed relative field is anchored at that field, not the record. */
#define TEAPOT_FAULT_SITE_MAGIC 0x53465054 /* TPFS */
#define TEAPOT_FAULT_SITE_VERSION 2
#define TEAPOT_FAULT_SITE_HEADER_SIZE 112
#define TEAPOT_FAULT_SITE_ENTRY_SIZE 16
#define TEAPOT_FAULT_SITE_TRAINING_ONLY 1
#define TEAPOT_FAULT_LOW_BOUND 65536
#define TEAPOT_FAULT_WINDOW_VERSION 3
#define TEAPOT_FAULT_WINDOW_ENTRY_SIZE 128
#define TEAPOT_FAULT_X64_WINDOWS 2
#define TEAPOT_FAULT_LOW_POLICY_VERSION 1 /* verified, page-rounded host bound */
#define TEAPOT_FAULT_MAX_WINDOW 19
#define TEAPOT_FAULT_RISC_VERSION 4
#define TEAPOT_FAULT_RISC_ENTRY_SIZE 128
#define TEAPOT_FAULT_RISC_WINDOWS 2
#define TEAPOT_FAULT_RISC_ISOLATION 65536
#define TEAPOT_FAULT_RISC_RECIPE_VERSION 1
#define TEAPOT_FAULT_RISC_ASSEMBLY_SCOPE_VERSION 1
#if defined(__aarch64__)
#define FAULT_RISC_NATIVE 1
#elif defined(__riscv) && __riscv_xlen == 64
#define FAULT_RISC_NATIVE 2
#else
#define FAULT_RISC_NATIVE 0
#endif
#if defined(__x86_64__)
#define TEAPOT_FAULT_PUBLISHER_VERSION TEAPOT_FAULT_WINDOW_VERSION
#else
#define TEAPOT_FAULT_PUBLISHER_VERSION TEAPOT_FAULT_RISC_VERSION
#endif

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>

struct teapot_fault_site_entry {
    int32_t fault_pc; /* Original, patchable memory-access instruction. */
    int32_t stub;
    int32_t copy_pc; /* Same access in the cold stub; shares the site's counter. */
    uint16_t length, flags; /* Length of both instructions; reserved flags = 0. */
};

struct teapot_fault_site_table {
    uint32_t magic;
    uint16_t version, header_size;
    uint32_t entry_size, count;
    uint32_t flags, threshold; /* 0 disables training; otherwise 1..255. */
    int64_t text_start, text_end;
    int64_t stub_start, stub_end;
    int64_t counters_start, counters_end;
    int64_t pending_start, pending_end;
    uint64_t reserved[3];
    struct teapot_fault_site_entry entries[];
};

/* A publisher must not accept training-only v2 entries. Every address is
 * relative to its own field, including the private three-word spill area.
 * The original bytes bind the patch to the validated instruction window;
 * at most one tail instruction has a RIP-relative disp32 (rip_offset/end).
 * All unused bytes are zero. Final-link validation additionally decodes the
 * whole window, checks its block and all incoming references, and compares
 * the effective address to the exact guard template. */
struct teapot_fault_window_entry {
    struct teapot_fault_site_entry site;
    int32_t return_pc, copy_end, block_start, block_end;
    int32_t spill, low, rollback;
    uint8_t access_length, address_width, base, index, scale, origin;
    uint8_t rip_offset, rip_end;
    uint32_t padding;
    int64_t displacement;
    uint8_t original[24];
    int32_t rip_target; /* Self-relative; the recorded disp32 itself is zero. */
    uint8_t reserved[36];
};
_Static_assert(sizeof(struct teapot_fault_window_entry) == TEAPOT_FAULT_WINDOW_ENTRY_SIZE, "window entry size");

/* RISC's whole four-byte instruction, never an x64 window or a NOP slot.
 * All targets are field-relative. Dead-bootstrap provenance is checked by the
 * emitter against validated original-input DDisasm masks before any raw split;
 * consumers validate the exact instruction, recipe, template and final reach. */
struct teapot_fault_risc_entry {
    struct teapot_fault_site_entry site;
    int32_t return_pc, copy_end, block_start, block_end;
    int32_t spill, policy, rollback, stub_end;
    uint32_t original;
    uint8_t origin, width, base, index, extension, shift, destination;
    uint8_t bootstrap, temp0, temp1, kind, padding;
    int64_t displacement;
    uint16_t spill_size, template_id;
    uint8_t reserved[52];
};
_Static_assert(sizeof(struct teapot_fault_risc_entry) == TEAPOT_FAULT_RISC_ENTRY_SIZE, "RISC entry size");
_Static_assert(offsetof(struct teapot_fault_risc_entry, original) == 48 &&
               offsetof(struct teapot_fault_risc_entry, displacement) == 64 &&
               offsetof(struct teapot_fault_risc_entry, reserved) == 76, "RISC entry offsets");

struct teapot_fault_risc_policy { uintptr_t low, high; };
#if FAULT_RISC_NATIVE
extern struct teapot_fault_risc_policy teapot_fault_risc_policy;
#endif

_Static_assert(sizeof(struct teapot_fault_site_entry) == TEAPOT_FAULT_SITE_ENTRY_SIZE, "fault entry size");
_Static_assert(sizeof(struct teapot_fault_site_table) == TEAPOT_FAULT_SITE_HEADER_SIZE, "fault header size");
_Static_assert(__GCC_ATOMIC_INT_LOCK_FREE == 2 && sizeof(unsigned int) == 4,
               "signal training requires lock-free 32-bit atomics, never libatomic helpers");

enum teapot_fault_low_failure {
    TEAPOT_FAULT_LOW_UNKNOWN = 1,
    TEAPOT_FAULT_LOW_SYSCTL = 2,
    TEAPOT_FAULT_LOW_CAPABILITY = 4,
    TEAPOT_FAULT_LOW_PERSONALITY = 8,
    TEAPOT_FAULT_LOW_MAPPING = 16,
    TEAPOT_FAULT_LOW_DISABLED = 32,
};

struct teapot_fault_low_facts {
    bool known;
    bool enabled;
    bool cap_sys_rawio;
    bool mmap_page_zero;
    bool mapped_below_bound;
    uint64_t mmap_min_addr;
};

struct teapot_fault_low_policy {
    uintptr_t bound;
    unsigned failures;
};

/* Pure decision helper and allocation-free raw startup collector. It gathers
 * the same facts on/off; failure/unknown disables the low slice. Later low
 * mappings/capability changes are unsupported while adaptation is on. */
struct teapot_fault_low_policy teapot_fault_low_policy(const struct teapot_fault_low_facts *, uintptr_t bound);
struct teapot_fault_low_policy teapot_fault_verify_low(uintptr_t bound, bool enabled);
bool teapot_fault_resolve_relative(uintptr_t anchor, int64_t relative, uintptr_t *result);

/* Called only after the module contract walk succeeds, before instrumentation
 * starts. A bounded, statically reserved protected-BSS snapshot is mprotected
 * read-only; no heap allocations or new mappings may prime application state.
 * Table/counter lifetimes must cover the process (dlopen/dlclose unsupported).
 * Capacity is 256 modules / 1,048,576 sites; overflow refuses before simulation. */
void teapot_fault_registry_initialize(const void *records_begin, const void *records_end);
/* .preinit_array runs before libc necessarily installs environ/getenv. The
 * ELF preinit arguments carry the real process environment at that point. */
void teapot_fault_registry_initialize_environment(const void *records_begin, const void *records_end,
                                                 char *const *environment);
bool teapot_fault_lookup(uintptr_t pc, size_t *module, size_t *site);
/* Only for synchronous kernel signals. A copied v4 load may have overwritten
 * its destination with a private address. Recognize its PC without inspecting
 * any GPR, and raw-stop an impossible inactive/replay context before forwarding
 * or diagnostics. Nonpositive-code notifications keep baseline routing. */
#if FAULT_RISC_NATIVE
bool teapot_fault_risc_copied_kernel(uintptr_t pc, bool active_checkpoint, bool restoring_memlog)
    __attribute__((visibility("hidden")));
#endif
bool teapot_fault_train(int sig, const siginfo_t *info, uintptr_t pc,
                        bool active_checkpoint, bool restoring_memlog);
uint8_t teapot_fault_counter(size_t module, size_t site);

struct teapot_fault_event {
    size_t module, site;
    uintptr_t fault_pc, stub, copy_pc;
    uint16_t length;
    uint8_t count;
};

extern uintptr_t teapot_fault_low_bound;
/* No scan and no signal-mask syscall when there is no pending event. The
 * caller is the common post-memlog rollback safe point (also restart path). */
void teapot_fault_publish_pending(void);
/* Pure template reconstruction shared by startup and native differential
 * tests. Returns 0 on range/shape failure, otherwise the full stub size. */
size_t teapot_fault_x64_template(const struct teapot_fault_window_entry *,
                                unsigned char *bytes, size_t capacity);
bool teapot_fault_x64_window(const struct teapot_fault_window_entry *, uintptr_t pc,
                            unsigned char *bytes, size_t capacity);
/* The high predicate requires startup-proven, unmasked x64 addresses. An
 * unavailable query or enabled LAM disables publishing, not merely low EA. */
bool teapot_fault_x64_can_publish_addresses(void);
/* Hidden direct call: publication must not fetch a libc PLT entry while
 * patch pages are RW/non-executable. Returns Linux's raw result (0/-errno). */
long teapot_fault_x64_mprotect(void *address, size_t length, int protection)
    __attribute__((visibility("hidden")));

/* At a trusted non-speculative safe point with signals blocked. Claims one
 * pending event exactly once; it does not publish a branch or alter text. */
bool teapot_fault_next_pending(struct teapot_fault_event *);
struct teapot_fault_low_policy teapot_fault_current_low_policy(void);

/* Pure validation/lookup helpers are shared with startup and focused tests.
 * The caller supplies a readable table extent and its protected counter area.
 * Registry initialization additionally verifies real ELF mapping permissions. */
bool teapot_fault_validate_table(const struct teapot_fault_site_table *, size_t readable_bytes,
                                 uintptr_t protected_start, uintptr_t protected_end);
bool teapot_fault_table_lookup(const struct teapot_fault_site_table *, uintptr_t pc, size_t *site);
uint8_t teapot_fault_increment(uint32_t *words, size_t site);
bool teapot_fault_mark_pending(uint32_t *words, size_t site);
bool teapot_fault_claim_pending(uint32_t *words, size_t site);
#endif
