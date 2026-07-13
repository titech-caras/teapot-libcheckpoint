#include "checkpoint.h"
#include "signal_handler.h"
#include "dift_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <unistd.h>

#if defined(__aarch64__) && defined(TEAPOT_AARCH64_MTE_TAG_STORAGE)
#ifndef PROT_MTE
#define PROT_MTE 0x20
#endif
#ifndef HWCAP2_MTE
#define HWCAP2_MTE (1UL << 18)
#endif
#ifndef PR_SET_TAGGED_ADDR_CTRL
#define PR_SET_TAGGED_ADDR_CTRL 55
#endif
#ifndef PR_TAGGED_ADDR_ENABLE
#define PR_TAGGED_ADDR_ENABLE (1UL << 0)
#endif
#ifndef PR_MTE_TCF_NONE
#define PR_MTE_TCF_NONE 0UL
#endif
#ifndef PR_MTE_TAG_SHIFT
#define PR_MTE_TAG_SHIFT 3
#endif
#ifndef PR_MTE_TAG_MASK
#define PR_MTE_TAG_MASK (0xffffUL << PR_MTE_TAG_SHIFT)
#endif
#endif

#if defined(__GNUC__) && !defined(__clang__) && \
        (defined(__aarch64__) || (defined(__riscv) && __riscv_xlen == 64))
#define LIBCHECKPOINT_RESTORE_PATH __attribute__((noinline, optimize("O0")))
#else
#define LIBCHECKPOINT_RESTORE_PATH __attribute__((noinline))
#endif

extern char __start_teapot_protected[];
extern char __stop_teapot_protected[];

checkpoint_metadata_t checkpoint_metadata[MAX_CHECKPOINTS] LIBCHECKPOINT_PROTECTED_SECTION;
xsave_area_t processor_extended_states[MAX_CHECKPOINTS] LIBCHECKPOINT_PROTECTED_SECTION;

memory_history_t memory_history[MEM_HISTORY_LEN] LIBCHECKPOINT_PROTECTED_SECTION;
memory_history_t *memory_history_top LIBCHECKPOINT_PROTECTED_SECTION = &memory_history[0];
uint32_t guard_list[GUARD_LIST_LEN] LIBCHECKPOINT_PROTECTED_SECTION;
uint32_t *guard_list_top LIBCHECKPOINT_PROTECTED_SECTION = &guard_list[0];

scratchpad_t scratchpad LIBCHECKPOINT_PROTECTED_SECTION;
void *old_rsp LIBCHECKPOINT_PROTECTED_SECTION;
void *scratchpad_rsp LIBCHECKPOINT_PROTECTED_SECTION;

statistics_t simulation_statistics LIBCHECKPOINT_PROTECTED_SECTION;

uint64_t last_rdtsc LIBCHECKPOINT_PROTECTED_SECTION = 0;
uint64_t checkpoint_cnt LIBCHECKPOINT_PROTECTED_SECTION = 0;
uint64_t instruction_cnt LIBCHECKPOINT_PROTECTED_SECTION = 0;
uint64_t indirect_branch_flags_scratch LIBCHECKPOINT_PROTECTED_SECTION = 0;

bool libcheckpoint_enabled LIBCHECKPOINT_PROTECTED_SECTION = false;
static bool libcheckpoint_runtime_initialized LIBCHECKPOINT_PROTECTED_SECTION = false;
volatile bool in_restore_memlog LIBCHECKPOINT_PROTECTED_SECTION = false;

__attribute__((weak)) void __asan_init(void);
__attribute__((weak)) void __asan_poison_memory_region(void const volatile *addr, size_t size);

#if defined(__aarch64__) && defined(TEAPOT_AARCH64_MTE_TAG_STORAGE)
static inline void mte_store_tag(uintptr_t addr, uint8_t tag);
#endif

#ifdef COVERAGE
__attribute__((weak)) void hfuzz_trace_pc(uint64_t pc) {
    (void)pc;
}

__attribute__((weak)) void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop) {
    (void)start;
    (void)stop;
}

__attribute__((weak)) void __sanitizer_cov_trace_pc_guard(uint32_t *guard) {
    (void)guard;
}

extern uint32_t guard_start asm("__guard_start__teapot__");
extern uint32_t guard_end asm("__guard_end__teapot__");
#endif

static uint64_t checkpoint_read_timer() {
#if defined(__x86_64__)
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t value;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#elif defined(__riscv) && __riscv_xlen == 64
    uint64_t value;
    asm volatile("rdcycle %0" : "=r"(value));
    return value;
#else
#error "Unsupported libcheckpoint timer architecture"
#endif
}

void poison_protected_zone() {
    uintptr_t protected_start = (uintptr_t)__start_teapot_protected;
    uintptr_t protected_end = (uintptr_t)__stop_teapot_protected;
    if (protected_end <= protected_start) {
        return;
    }

#if defined(__aarch64__) && defined(TEAPOT_AARCH64_MTE_TAG_STORAGE)
    uintptr_t aligned_start = protected_start & ~(uintptr_t)0xf;
    for (uintptr_t addr = aligned_start; addr < protected_end; addr += 16) {
        mte_store_tag(addr, 0xf);
    }
#else
    if (!__asan_poison_memory_region) {
        fputs("Teapot instrumented binaries must provide __asan_poison_memory_region\n", stderr);
        abort();
    }

    __asan_poison_memory_region((void *)protected_start, protected_end - protected_start);
#endif
}

#if defined(__aarch64__) && defined(TEAPOT_AARCH64_MTE_TAG_STORAGE)
static inline void mte_store_tag(uintptr_t addr, uint8_t tag) {
    uintptr_t aligned = DIFT_APP_ADDR(addr) & ~(uintptr_t)0xf;
    uintptr_t tagged = aligned | ((uintptr_t)(tag & 0xf) << 56);
    asm volatile("stg %0, [%1]" :: "r"(tagged), "r"(aligned) : "memory");
}

static int maps_protection_from_perms(const char *perms) {
    int prot = 0;
    if (perms[0] == 'r')
        prot |= PROT_READ;
    if (perms[1] == 'w')
        prot |= PROT_WRITE;
    if (perms[2] == 'x')
        prot |= PROT_EXEC;
    return prot;
}

static void enable_mte_for_mapped_app_overlap(uintptr_t app_start, uintptr_t app_end) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps == NULL) {
        perror("open /proc/self/maps for MTE setup");
        abort();
    }

    char line[512];
    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long long range_start;
        unsigned long long range_end;
        char perms[5] = {0};
        if (sscanf(line, "%llx-%llx %4s", &range_start, &range_end, perms) != 3)
            continue;
        if (perms[1] != 'w')
            continue;

        uintptr_t start = (uintptr_t)range_start;
        uintptr_t end = (uintptr_t)range_end;
        if (end <= app_start || start >= app_end)
            continue;
        if (start < app_start)
            start = app_start;
        if (end > app_end)
            end = app_end;
        if (start >= end)
            continue;

        int prot = maps_protection_from_perms(perms) | PROT_MTE;
        if (mprotect((void *)start, end - start, prot) != 0) {
            fprintf(stderr, "mprotect(PROT_MTE) 0x%llx-0x%llx failed: %s\n",
                    (unsigned long long)start,
                    (unsigned long long)end,
                    strerror(errno));
            abort();
        }
    }

    fclose(maps);
}

static void initialize_aarch64_mte_tag_storage() {
    if (!(getauxval(AT_HWCAP2) & HWCAP2_MTE)) {
        fputs("TEAPOT_AARCH64_MTE_TAG_STORAGE requires AArch64 MTE support\n", stderr);
        abort();
    }

    unsigned long ctrl = PR_TAGGED_ADDR_ENABLE | PR_MTE_TCF_NONE | PR_MTE_TAG_MASK;
    if (prctl(PR_SET_TAGGED_ADDR_CTRL, ctrl, 0, 0, 0) != 0) {
        fprintf(stderr, "prctl(PR_SET_TAGGED_ADDR_CTRL, MTE no-fault mode) failed: %s\n",
                strerror(errno));
        abort();
    }

#ifdef DIFT_APP_RANGE0_START
    enable_mte_for_mapped_app_overlap(DIFT_APP_RANGE0_START, DIFT_APP_RANGE0_END);
#endif
#ifdef DIFT_APP_RANGE1_START
    enable_mte_for_mapped_app_overlap(DIFT_APP_RANGE1_START, DIFT_APP_RANGE1_END);
#endif
#ifdef DIFT_APP_RANGE2_START
    enable_mte_for_mapped_app_overlap(DIFT_APP_RANGE2_START, DIFT_APP_RANGE2_END);
#endif
#ifdef DIFT_APP_RANGE3_START
    enable_mte_for_mapped_app_overlap(DIFT_APP_RANGE3_START, DIFT_APP_RANGE3_END);
#endif
#ifdef DIFT_APP_RANGE4_START
    enable_mte_for_mapped_app_overlap(DIFT_APP_RANGE4_START, DIFT_APP_RANGE4_END);
#endif
}
#endif

static void initialize_first_spill_state() {
#if defined(__riscv) && __riscv_xlen == 64
    uintptr_t original_tp;
    asm volatile("mv %0, tp" : "=r"(original_tp));
    *(uint64_t *)(scratchpad + RISCV64_ORIGINAL_TP_OFFSET) = original_tp;
#endif
}

#if defined(__riscv) && __riscv_xlen == 64
__attribute__((constructor(101))) static void initialize_first_spill_state_early() {
    initialize_first_spill_state();
}
#endif

static void initialize_shadow_stack_state() {
#if defined(__aarch64__)
#ifndef AARCH64_SHADOW_STACK_SIZE
#define AARCH64_SHADOW_STACK_SIZE (8ULL * 1024ULL * 1024ULL)
#endif
    volatile uint8_t stack_marker;
    uintptr_t sp = (uintptr_t)&stack_marker;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        page_size = 4096;

    uintptr_t page_mask = (uintptr_t)page_size - 1;
    uintptr_t stack_mapping_start = sp & ~page_mask;
    uintptr_t stack_mapping_end = (sp + page_mask) & ~page_mask;

    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps != NULL) {
        char line[256];
        while (fgets(line, sizeof(line), maps) != NULL) {
            unsigned long long range_start;
            unsigned long long range_end;
            if (sscanf(line, "%llx-%llx", &range_start, &range_end) != 2)
                continue;
            if ((uintptr_t)range_start <= sp && sp < (uintptr_t)range_end) {
                stack_mapping_start = (uintptr_t)range_start;
                stack_mapping_end = (uintptr_t)range_end;
                break;
            }
        }
        fclose(maps);
    }

    uintptr_t app_stack_size = (uintptr_t)AARCH64_SHADOW_STACK_SIZE;
    if (stack_mapping_end < 2 * app_stack_size) {
        fputs("AArch64 shadow stack window underflows the address space\n", stderr);
        abort();
    }

    uintptr_t app_stack_start = stack_mapping_end - app_stack_size;
    uintptr_t helper_stack_start = stack_mapping_end - 2 * app_stack_size;
    uintptr_t helper_stack_end = app_stack_start;

    if (helper_stack_start < stack_mapping_start) {
        uintptr_t map_end = helper_stack_end < stack_mapping_start ?
            helper_stack_end : stack_mapping_start;
        map_runtime_shadow_range(helper_stack_start, map_end, PROT_READ | PROT_WRITE);
    }
#endif
}

static bool memory_history_entry_is_valid(const memory_history_t *entry) {
#if defined(__aarch64__) && defined(TEAPOT_AARCH64_MTE_TAG_STORAGE)
    if (entry->size == MEM_HISTORY_MTE_TAG_SIZE)
        return true;
#endif
    return entry->size > 0 && entry->size <= sizeof(entry->data);
}

static void validate_memory_history_range(memory_history_t *checkpoint_top) {
    if (checkpoint_top < &memory_history[0] ||
            checkpoint_top > &memory_history[MEM_HISTORY_LEN]) {
        fputs("Checkpoint memory history pointer is outside runtime history\n", stderr);
        abort();
    }

    if (memory_history_top < checkpoint_top ||
            memory_history_top > &memory_history[MEM_HISTORY_LEN]) {
        fputs("Current memory history pointer is outside runtime history\n", stderr);
        abort();
    }

    for (memory_history_t *entry = memory_history_top; entry > checkpoint_top;) {
        entry--;
        if (!memory_history_entry_is_valid(entry)) {
            fputs("Invalid memory history entry during rollback\n", stderr);
            abort();
        }
    }
}

void print_statistics() {
    fprintf(stderr, "Total Checkpoints: %lu\n", simulation_statistics.total_ckpt);

    for (int i = 0; i < MAX_CHECKPOINTS; i++) {
        fprintf(stderr, "\tDepth %d: %lu\n", i + 1, simulation_statistics.ckpt_depth[i]);
    }

    puts("");
    puts("Rollbacks");
    fprintf(stderr, "\tRollback ROB_LEN: %lu\n", simulation_statistics.rollback_reason[ROLLBACK_ROB_LEN]);
    //fprintf(stderr, "\tRollback ASAN: %lu\n", simulation_statistics.rollback_reason[ROLLBACK_ASAN]);
    fprintf(stderr, "\tRollback SIGSEGV: %lu\n", simulation_statistics.rollback_reason[ROLLBACK_SIGSEGV]);
    fprintf(stderr, "\tRollback EXT_LIB: %lu\n", simulation_statistics.rollback_reason[ROLLBACK_EXT_LIB]);
    fprintf(stderr, "\tRollback MALFORMED_INDIRECT_BR: %lu\n", simulation_statistics.rollback_reason[ROLLBACK_MALFORMED_INDIRECT_BR]);

    puts("");
    fprintf(stderr, "Total Bugs: %lu\n", simulation_statistics.total_bug);
    fprintf(stderr, "\tBug KASPER_MDS: %lu\n", simulation_statistics.bug_type[GADGET_KASPER_MDS]);
    fprintf(stderr, "\tBug KASPER_CACHE: %lu\n", simulation_statistics.bug_type[GADGET_KASPER_CACHE]);
    fprintf(stderr, "\tBug KASPER_PORT: %lu\n", simulation_statistics.bug_type[GADGET_KASPER_PORT]);

#ifdef TIME

    uint64_t total_time = simulation_statistics.rdtsc_runtime.normal_time +
            simulation_statistics.rdtsc_runtime.spec_time +
            simulation_statistics.rdtsc_runtime.ckpt_time +
            simulation_statistics.rdtsc_runtime.rstr_time;
    fprintf(stderr, "Time spent %%: Normal: %.2lf%% / Spec: %.2lf%% / Ckpt: %.2lf%% / Rstr: %.2lf%%\n",
            simulation_statistics.rdtsc_runtime.normal_time * 100.0 / total_time,
            simulation_statistics.rdtsc_runtime.spec_time * 100.0 / total_time,
            simulation_statistics.rdtsc_runtime.ckpt_time * 100.0 / total_time,
            simulation_statistics.rdtsc_runtime.rstr_time * 100.0 / total_time);
#endif
}

static void libcheckpoint_prepare_runtime(int argc, char **argv) {
    if (libcheckpoint_runtime_initialized)
        return;

#if defined(__aarch64__) && defined(TEAPOT_AARCH64_MTE_TAG_STORAGE)
    initialize_aarch64_mte_tag_storage();
#else
    if (!__asan_init) {
        fputs("Teapot instrumented binaries must be linked with AddressSanitizer\n", stderr);
        abort();
    }
#endif

#ifdef COVERAGE
    if (__sanitizer_cov_trace_pc_guard_init) {
        __sanitizer_cov_trace_pc_guard_init(&guard_start, &guard_end);
    }
#endif

    poison_protected_zone();
    initialize_first_spill_state();
    initialize_shadow_stack_state();
#ifndef DISABLE_DIFT_RUNTIME
    map_dift_pages();
    dift_taint_args(argc, argv);
#else
    (void)argc;
    (void)argv;
#endif
    setup_signal_handler();
    libcheckpoint_runtime_initialized = true;
}

LIBCHECKPOINT_PRESERVE_MOST void libcheckpoint_enable(int argc, char **argv) {
    if (libcheckpoint_enabled)
        return;

    libcheckpoint_prepare_runtime(argc, argv);

    fprintf(stderr, "[teapot], "
        "Gadget Type, Gadget Address, Mem Access Address, "
        "Tag, Instruction Counter, Checkpoint Addresses\n");
    last_rdtsc = checkpoint_read_timer();
    libcheckpoint_enabled = true;
    atexit((void (*)(void)) libcheckpoint_disable);
}

LIBCHECKPOINT_PRESERVE_MOST void libcheckpoint_disable() {
    if (!libcheckpoint_enabled)
        return;

    libcheckpoint_enabled = false;

#ifdef VERBOSE
    print_statistics();
#endif
}

LIBCHECKPOINT_RESTORE_PATH __attribute__((noreturn)) void restore_checkpoint(int type) {
    assert(checkpoint_cnt > 0);

#ifdef TIME
    uint64_t rdtsc_time = checkpoint_read_timer();
    simulation_statistics.rdtsc_runtime.spec_time += rdtsc_time - last_rdtsc;
    last_rdtsc = rdtsc_time;
#endif

    simulation_statistics.total_ckpt++;
    simulation_statistics.ckpt_depth[checkpoint_cnt - 1]++;
    simulation_statistics.rollback_reason[type]++;

    checkpoint_cnt--;

#ifdef VERBOSE_DBGINFO
    fprintf(stderr, "[teapot] Rollback: to 0x%lx at nested level %lu\n",
            checkpoint_metadata[checkpoint_cnt].return_address, checkpoint_cnt);
#endif

#ifdef COVERAGE
    if (__sanitizer_cov_trace_pc_guard) {
        uint32_t *checkpoint_guard_top = checkpoint_metadata[checkpoint_cnt].guard_list_top;
        if (checkpoint_guard_top < &guard_list[0] ||
                checkpoint_guard_top > &guard_list[GUARD_LIST_LEN]) {
            checkpoint_guard_top = &guard_list[0];
        }
        if (guard_list_top < checkpoint_guard_top ||
                guard_list_top > &guard_list[GUARD_LIST_LEN]) {
            guard_list_top = checkpoint_guard_top;
        }
        size_t guard_count = (size_t)(&guard_end - &guard_start);
        while (guard_list_top > checkpoint_guard_top) {
            guard_list_top--;
            uint32_t guard_idx = *guard_list_top;
            if (guard_idx >= guard_count) continue;
            uint32_t *guard_ptr = &guard_start + guard_idx;
            if (!*guard_ptr) continue;
            __sanitizer_cov_trace_pc_guard(guard_ptr);
        }
    } else {
        guard_list_top = checkpoint_metadata[checkpoint_cnt].guard_list_top;
    }
#else
    guard_list_top = checkpoint_metadata[checkpoint_cnt].guard_list_top;
#endif

    restore_checkpoint_memlog();
    restore_checkpoint_after_memlog();
}

LIBCHECKPOINT_RESTORE_PATH __attribute__((noreturn)) void restore_checkpoint_after_memlog() {
    instruction_cnt = checkpoint_metadata[checkpoint_cnt].instruction_cnt;
    for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) {
        dift_reg_tags[i] = checkpoint_metadata[checkpoint_cnt].dift_reg_tags[i];
    }

    restore_checkpoint_registers();
}

LIBCHECKPOINT_RESTORE_PATH void restore_checkpoint_memlog() {
    in_restore_memlog = true;
    validate_memory_history_range(checkpoint_metadata[checkpoint_cnt].memory_history_top);
    while (memory_history_top > checkpoint_metadata[checkpoint_cnt].memory_history_top) {
        // This may fail if the address is only readable. SIGSEGV handler detects this and the entry will be skipped.
        memory_history_top--;
#if defined(__aarch64__) && defined(TEAPOT_AARCH64_MTE_TAG_STORAGE)
        if (memory_history_top->size == MEM_HISTORY_MTE_TAG_SIZE) {
            mte_store_tag((uintptr_t)memory_history_top->addr,
                    (uint8_t)memory_history_top->data);
            memory_history_top->size = 0;
            continue;
        }
#endif
        volatile uint8_t *dst = (volatile uint8_t *)memory_history_top->addr;
        const uint8_t *src = (const uint8_t *)&memory_history_top->data;
        size_t size = memory_history_top->size;
        for (size_t i = 0; i < size; i++) {
            dst[i] = src[i];
        }
        memory_history_top->size = 0;
    }
    in_restore_memlog = false;
}
