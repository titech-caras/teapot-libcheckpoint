#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include "checkpoint.h"
#include "signal_handler.h"

extern uint64_t max_checkpoints;
extern uintptr_t checkpoint_target_metadata[CHECKPOINT_TARGET_METADATA_SIZE / 8];
extern int checkpoint_entry_probe(void *stack_top);
extern int checkpoint_rollback_probe(void *stack_top);
#if defined(__x86_64__)
extern void check_x64_df(void);
#endif
extern __attribute__((noreturn)) void restore_checkpoint_ROB_LEN(void);
extern memory_history_t *memory_history_top;
extern uint32_t *guard_list_top;
extern void poison_protected_zone(void);
extern char __start_teapot_protected[], __stop_teapot_protected[];
extern char __start_teapot_protected_bss[], __stop_teapot_protected_bss[];
#if defined(__aarch64__)
extern int checkpoint_report_probe(void *stack_top);
extern int check_aarch64_bti_fault(void *stack_top);
extern int check_aarch64_bti_live_chain(void *stack_top);
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
extern int check_aarch64_bti_backend(void *stack_top, bool live_chain);
extern int check_aarch64_bti_traps(void *stack_top);
#endif
#endif

uint32_t __guard_start__teapot__[1];
uint32_t __guard_end__teapot__[1];
static uint32_t branch_counter;
static void *stack_top;
static unsigned poisoned_ranges;
static sigjmp_buf memlog_fault_return;
static void *queue_fault_page;
static bool queue_force_fault;

/* The assembly probe tail-enters this only after a real checkpoint. */
__attribute__((noreturn)) void checkpoint_test_transient_body(void) {
    memset(dift_reg_queued_tags, TAG_SECRET_INDIRECT, DIFT_REG_TAGS_SIZE);
    memset(dift_reg_tags, TAG_SECRET, DIFT_REG_TAGS_SIZE);
    /* The queue must be empty even if replay itself writes nonzero bytes. */
    memory_history[0] = (memory_history_t){
        .addr = dift_reg_queued_tags, .data = UINT64_MAX, .size = 8};
    memory_history_top = memory_history + 1;
    if (queue_force_fault) {
        memory_history[1] = (memory_history_t){
            .addr = queue_fault_page, .data = UINT64_MAX, .size = 8};
        memory_history_top++;
    }
    restore_checkpoint_ROB_LEN();
}

static void check_queued_tags(void) {
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    queue_fault_page = mmap(NULL, page_size, PROT_READ,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(queue_fault_page != MAP_FAILED);
    setup_signal_handler();
    for (unsigned fault = 0; fault < 2; ++fault) {
        queue_force_fault = fault;
        for (unsigned depth = 0; depth < MAX_CHECKPOINTS; ++depth) {
            checkpoint_cnt = depth;
            max_checkpoints = MAX_CHECKPOINTS;
            branch_counter = 0;
            memory_history_top = memory_history;
            guard_list_top = guard_list;
            for (unsigned i = 0; i < DIFT_REG_TAGS_SIZE; ++i) {
                dift_reg_tags[i] = (dift_tag_t)(i + 1);
                dift_reg_queued_tags[i] = 0;
            }
            assert(checkpoint_rollback_probe(stack_top) == 1);
            assert(checkpoint_cnt == depth && !in_restore_memlog);
            assert(memory_history_top == memory_history);
            for (unsigned i = 0; i < DIFT_REG_TAGS_SIZE; ++i) {
                assert(dift_reg_tags[i] == i + 1);
                assert(dift_reg_queued_tags[i] == 0);
            }
        }
    }
    checkpoint_cnt = 0;
    assert(munmap(queue_fault_page, page_size) == 0);
}

static void memlog_fault_handler(int signal) {
    assert(signal == SIGSEGV || signal == SIGBUS);
    siglongjmp(memlog_fault_return, 1);
}

static void check_memlog(void) {
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);
    memset(page, 0x55, page_size);
    assert(mprotect(page, page_size, PROT_READ) == 0);
    static unsigned char bytes[16];
    checkpoint_cnt = 0;
    checkpoint_metadata[0].memory_history_top = memory_history;
    memory_history[0] = (memory_history_t){.addr = bytes, .data = UINT64_MAX, .size = 8};
    memory_history[1] = (memory_history_t){.addr = page, .data = UINT64_MAX, .size = 8};
    memory_history[2] = (memory_history_t){.addr = bytes + 8, .data = UINT64_MAX, .size = 8};
    memory_history_top = memory_history + 3;
    struct sigaction action = {.sa_handler = memlog_fault_handler}, old_segv, old_bus;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGSEGV, &action, &old_segv) == 0);
    assert(sigaction(SIGBUS, &action, &old_bus) == 0);
    if (sigsetjmp(memlog_fault_return, 1) == 0) {
        restore_checkpoint_memlog();
        abort();
    }
    assert(in_restore_memlog && memory_history_top == memory_history + 1);
    restore_checkpoint_memlog();
    assert(!in_restore_memlog && memory_history_top == memory_history);
    for (size_t i = 0; i < sizeof(bytes); i++) assert(bytes[i] == 0xff);
    assert(page[0] == 0x55);
    assert(sigaction(SIGSEGV, &old_segv, NULL) == 0);
    assert(sigaction(SIGBUS, &old_bus, NULL) == 0);
    assert(munmap(page, page_size) == 0);
}

/* Verify exact calls without adding ASan's startup requirements to the
 * assembly-entry tests. Full-pipeline smokes link the actual ASan runtime. */
void __asan_poison_memory_region(void const volatile *address, size_t size) {
    assert(poisoned_ranges < 2);
    uintptr_t start = (uintptr_t)(poisoned_ranges ? __start_teapot_protected_bss : __start_teapot_protected);
    uintptr_t end = (uintptr_t)(poisoned_ranges ? __stop_teapot_protected_bss : __stop_teapot_protected);
    assert((uintptr_t)address == start && size == end - start);
    assert(start % 8 == 0 && end % 8 == 0);
    assert((uintptr_t)&poisoned_ranges < start || (uintptr_t)&poisoned_ranges >= end);
    poisoned_ranges++;
}

static void check_storage(void) {
    assert(memory_history_top == memory_history && guard_list_top == guard_list);
    const struct {
        void *address;
        size_t size;
    } objects[] = {
        {memory_history, sizeof(memory_history)},
        {guard_list, sizeof(guard_list)},
        {scratchpad, sizeof(scratchpad)},
    };
    uintptr_t start = (uintptr_t)__start_teapot_protected_bss;
    uintptr_t end = (uintptr_t)__stop_teapot_protected_bss;
    assert(end - start == sizeof(memory_history) + sizeof(guard_list) + sizeof(scratchpad));
    for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); i++) {
        uintptr_t address = (uintptr_t)objects[i].address;
        assert(start <= address && address + objects[i].size <= end);
        assert(address % 16 == 0);
        const unsigned char *bytes = objects[i].address;
        for (size_t offset = 0; offset < objects[i].size; offset++) {
#if defined(__riscv) && __riscv_xlen == 64
            if (objects[i].address == scratchpad && offset >= RISCV64_ORIGINAL_TP_OFFSET &&
                    offset < RISCV64_ORIGINAL_TP_OFFSET + sizeof(uintptr_t))
                continue; /* The real preinit routine has already recorded tp. */
#endif
            assert(bytes[offset] == 0);
        }
    }
#if defined(__riscv) && __riscv_xlen == 64
    uintptr_t tp;
    __asm__ volatile("mv %0, tp" : "=r"(tp));
    assert(*(uintptr_t *)&scratchpad[RISCV64_ORIGINAL_TP_OFFSET] == tp);
#endif
    poison_protected_zone();
#if defined(TEAPOT_AARCH64_MTE_TAG_STORAGE)
    assert(poisoned_ranges == 0);
#else
    assert(poisoned_ranges == 2);
#endif
    memory_history[MEM_HISTORY_LEN - 1].size = 8;
    guard_list[GUARD_LIST_LEN - 1] = 1;
    scratchpad[SCRATCHPAD_SIZE - 1] = 1;
}

static unsigned expected_limit(uint32_t count) {
#ifdef ENABLE_NESTED_SPECULATION
    if (count <= BRANCH_FULL_EXEC_COUNT)
        return MAX_CHECKPOINTS;
    unsigned trailing_zeros = 0;
    while (!(count & 1)) {
        count >>= 1;
        trailing_zeros++;
    }
    unsigned limit = trailing_zeros / 2 + 1;
    return limit < MAX_CHECKPOINTS ? limit : MAX_CHECKPOINTS;
#else
    (void)count;
    return 1;
#endif
}

static void check_capacity(void) {
    const uint32_t counts[] = {
        1, 5, 6, 8, 16, 64, 1024, UINT32_C(0x80000001), 4096, 16384,
        UINT32_C(0x80000000), UINT32_MAX, 0,
    };
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        unsigned limit = expected_limit(counts[i]);
        branch_counter = counts[i] - 1;
        checkpoint_cnt = 0;
        max_checkpoints = 0;
        assert(checkpoint_entry_probe(stack_top) == 1);
        if (max_checkpoints != limit) {
            fprintf(stderr, "count=%" PRIu32 " limit=%" PRIu64 " expected=%u\n",
                    counts[i], max_checkpoints, limit);
            abort();
        }
        assert(checkpoint_cnt == 1);
#if defined(__aarch64__)
        assert(checkpoint_metadata[0].registers.x16 == 0x1640);
        assert(checkpoint_metadata[0].registers.x17 == 0x1740);
#endif
#ifdef USE_BRANCH_EXEC_COUNT
        assert(branch_counter == counts[i]);
#endif
        uint32_t saved_counter = branch_counter;
        for (unsigned depth = 1; depth < limit; depth++) {
            assert(checkpoint_entry_probe(stack_top) == 1);
            assert(checkpoint_cnt == depth + 1);
            assert(branch_counter == saved_counter);
        }
        checkpoint_metadata_t saved[MAX_CHECKPOINTS];
        memcpy(saved, checkpoint_metadata, sizeof(saved));
        assert(checkpoint_entry_probe(stack_top) == 0);
        assert(checkpoint_cnt == limit);
        assert(memcmp(saved, checkpoint_metadata, sizeof(saved)) == 0);
    }
    libcheckpoint_enabled = false;
    checkpoint_cnt = 0;
    assert(checkpoint_entry_probe(stack_top) == 0);
    assert(checkpoint_cnt == 0);
}

static void check_timing(void) {
#if defined(__aarch64__)
    uint64_t original_fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(original_fpcr));
    const uint64_t fpcr_values[] = {0, UINT64_C(1) << 22};
#else
    const uint64_t fpcr_values[] = {0};
#endif
    for (size_t i = 0; i < sizeof(fpcr_values) / sizeof(fpcr_values[0]); i++) {
        for (unsigned depth = 0; depth < MAX_CHECKPOINTS; depth++) {
#if defined(__aarch64__)
            __asm__ volatile("msr fpcr, %0" :: "r"(fpcr_values[i]));
#endif
            memset(&simulation_statistics, 0, sizeof(simulation_statistics));
            last_rdtsc = 0;
            branch_counter = 0;
            checkpoint_cnt = depth;
            max_checkpoints = MAX_CHECKPOINTS;
            assert(checkpoint_entry_probe(stack_top) == 1);
            if ((simulation_statistics.rdtsc_runtime.normal_time != 0) != (depth == 0) ||
                    (simulation_statistics.rdtsc_runtime.spec_time != 0) != (depth != 0)) {
                fprintf(stderr, "depth=%u fpcr=%" PRIx64 " normal=%" PRIu64 " nested=%" PRIu64 "\n",
                        depth, fpcr_values[i], simulation_statistics.rdtsc_runtime.normal_time,
                        simulation_statistics.rdtsc_runtime.spec_time);
                abort();
            }
#if defined(__aarch64__)
            uint64_t restored_fpcr;
            __asm__ volatile("mrs %0, fpcr" : "=r"(restored_fpcr));
            assert(restored_fpcr == fpcr_values[i]);
#endif
        }
    }
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr, %0" :: "r"(original_fpcr));
#endif
}

int main(int argc, char **argv) {
    assert(argc == 2);
    struct rlimit core_limit = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &core_limit) == 0);
#if defined(__aarch64__)
    size_t stack_size = 2 * AARCH64_SHADOW_STACK_SIZE;
    void *mapping = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    stack_top = (char *)mapping + stack_size - 4096;
#endif
    checkpoint_target_metadata[CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR / 8] = (uintptr_t)&branch_counter;
    checkpoint_target_metadata[CHECKPOINT_TARGET_FIXED_REG0_SOURCE / 8] = UINTPTR_MAX;
    checkpoint_target_metadata[CHECKPOINT_TARGET_FIXED_REG1_SOURCE / 8] = UINTPTR_MAX;
    libcheckpoint_enabled = true;
    if (strcmp(argv[1], "capacity") == 0)
        check_capacity();
    else if (strcmp(argv[1], "timing") == 0)
        check_timing();
    else if (strcmp(argv[1], "storage") == 0)
        check_storage();
    else if (strcmp(argv[1], "memlog") == 0)
        check_memlog();
    else if (strcmp(argv[1], "queued-tags") == 0)
        check_queued_tags();
#if defined(__x86_64__)
    else if (strcmp(argv[1], "df") == 0)
        check_x64_df();
#endif
#if defined(__aarch64__)
    else if (strcmp(argv[1], "report") == 0)
        assert(checkpoint_report_probe(stack_top) == 1);
    else if (strcmp(argv[1], "bti-fault") == 0) {
        int status = check_aarch64_bti_fault(stack_top);
        assert(munmap(mapping, stack_size) == 0);
        return status;
    }
    else if (strcmp(argv[1], "bti-live-chain") == 0) {
        int status = check_aarch64_bti_live_chain(stack_top);
        assert(munmap(mapping, stack_size) == 0);
        return status;
    }
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
    else if (strcmp(argv[1], "bti-traps") == 0) {
        int status = check_aarch64_bti_traps(stack_top);
        assert(munmap(mapping, stack_size) == 0);
        return status;
    }
    else if (strcmp(argv[1], "bti-backend") == 0 ||
             strcmp(argv[1], "bti-backend-live-chain") == 0) {
        int status = check_aarch64_bti_backend(stack_top,
                         strcmp(argv[1], "bti-backend-live-chain") == 0);
        assert(munmap(mapping, stack_size) == 0);
        return status;
    }
#endif
#endif
    else
        return 2;
#if defined(__aarch64__)
    assert(munmap(mapping, stack_size) == 0);
#endif
    return 0;
}
