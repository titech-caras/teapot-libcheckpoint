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
#include <sys/wait.h>
#include <unistd.h>

#include "checkpoint.h"
#include "signal_handler.h"

extern uint64_t max_checkpoints;
extern uintptr_t checkpoint_target_metadata[CHECKPOINT_TARGET_METADATA_SIZE / 8];
extern int checkpoint_entry_probe(void *stack_top);
extern int checkpoint_rollback_probe(void *stack_top);
#if defined(__riscv)
/* Only the entry tests link tests/checkpoint_riscv64_float.c. */
extern void check_riscv64_float(void) __attribute__((weak));
#endif
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
extern int check_aarch64_pac(void *stack_top);
#endif
#endif

/* A rewritten module's coverage guards, as Teapot's guard section defines
 * them: one 32-bit guard per transient block, between the two symbols. */
#define COVERAGE_GUARDS 4
__asm__(".pushsection .data.checkpoint_test_guards,\"aw\"\n"
        ".balign 4\n"
        ".globl __guard_start__teapot__, __guard_end__teapot__\n"
        "__guard_start__teapot__:\n"
        ".zero 16\n"
        "__guard_end__teapot__:\n"
        ".popsection\n");
extern uint32_t __guard_start__teapot__[COVERAGE_GUARDS];
_Static_assert(COVERAGE_GUARDS == 4, "the assembly above reserves four guards");
static uint32_t branch_counter;
static void *stack_top;
static unsigned poisoned_ranges;
static sigjmp_buf memlog_fault_return;
static void *queue_fault_page;
static bool queue_force_fault;
static bool coverage_test;
static __attribute__((noreturn)) void coverage_transient_body(void);

static void check_assertions(void) {
    int output[2];
    assert(pipe(output) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(output[0]);
        assert(dup2(output[1], STDERR_FILENO) >= 0);
        close(output[1]);
        checkpoint_cnt = 0;
        /* Build configurations must not remove this safety precondition. */
        restore_checkpoint(ROLLBACK_ROB_LEN);
    }
    close(output[1]);
    char diagnostic[1024];
    size_t length = 0;
    ssize_t count;
    while ((count = read(output[0], diagnostic + length,
                         sizeof(diagnostic) - 1 - length)) > 0)
        length += (size_t)count;
    assert(count == 0);
    assert(length > 0);
    diagnostic[length] = '\0';
    close(output[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(strstr(diagnostic, "checkpoint_cnt > 0") != NULL);
}

/* The assembly probe tail-enters this only after a real checkpoint. */
__attribute__((noreturn)) void checkpoint_test_transient_body(void) {
    if (coverage_test)
        coverage_transient_body();
    memset(dift_reg_queued_tags, TAG_SECRET_INDIRECT, DIFT_REG_TAGS_SIZE);
    dift_reg_queue_pending[0] = 1;
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
            assert(dift_reg_queue_pending[0] == 0);
            for (unsigned i = 0; i < DIFT_REG_TAGS_SIZE; ++i) {
                assert(dift_reg_tags[i] == i + 1);
                assert(dift_reg_queued_tags[i] == 0);
            }
        }
    }
    checkpoint_cnt = 0;
    assert(munmap(queue_fault_page, page_size) == 0);
}

/*
 * Speculative coverage, as the fuzzer receives it (these programs define
 * COVERAGE). A rollback replays the guards its window pushed into the Sanitizer
 * Coverage callback, newest first, once the memory log is undone; guards pushed
 * before the window's checkpoint stay for the enclosing window. A guard the
 * fuzzer has zeroed, and an index outside the guard section, are skipped. With
 * nesting the chain is real: the outer window's body makes the inner checkpoint.
 */
static unsigned coverage_events[8], coverage_event_count, coverage_step;
static uint64_t coverage_word, coverage_word_expected;
static bool coverage_nested;

/* The fuzzer's callback, which the COVERAGE runtime requires: this test's
 * recording provider (tests/coverage_provider.c supplies the other two). */
void __sanitizer_cov_trace_pc_guard(uint32_t *guard) {
    assert(guard >= __guard_start__teapot__ && guard < __guard_start__teapot__ + COVERAGE_GUARDS);
    assert(*guard != 0);
    /* Speculation may have written the fuzzer's own memory: the replay must
     * see it restored. */
    assert(!in_restore_memlog && coverage_word == coverage_word_expected);
    assert(coverage_event_count < sizeof(coverage_events) / sizeof(coverage_events[0]));
    coverage_events[coverage_event_count++] = (unsigned)(guard - __guard_start__teapot__);
}

/* What Teapot's coverage push does at a transient block. */
static void coverage_push(uint32_t index) {
    *guard_list_top++ = index;
}

static void coverage_logged_write(uint64_t value) {
    *memory_history_top++ = (memory_history_t){.addr = &coverage_word, .data = coverage_word, .size = 8};
    coverage_word = value;
}

static __attribute__((noreturn)) void coverage_transient_body(void) {
    if (coverage_step++ == 0) {
        /* The outer window, or the only one. */
        coverage_logged_write(0x1111);
        coverage_push(0);
        coverage_push(3);
        coverage_push(COVERAGE_GUARDS);
        if (coverage_nested) {
            uint64_t depth = checkpoint_cnt;
            /* AArch64 probes take a stack; leave this body's frame alone. */
            void *inner_stack = stack_top ? (char *)stack_top - 65536 : NULL;
            coverage_word_expected = 0x1111;
            assert(checkpoint_rollback_probe(inner_stack) == 1);
            /* The inner rollback replayed only its own guard. */
            assert(checkpoint_cnt == depth && coverage_word == 0x1111);
            assert(coverage_event_count == 1 && coverage_events[0] == 2);
            assert(guard_list_top == guard_list + 3);
            coverage_push(1);
        }
        if (queue_force_fault) {
            /* The newest entry faults in the replay, which then restarts. */
            *memory_history_top++ = (memory_history_t){
                .addr = queue_fault_page, .data = UINT64_MAX, .size = 8};
        }
        coverage_word_expected = 0x5555;
        restore_checkpoint_ROB_LEN();
    }
    /* The inner window. */
    coverage_logged_write(0x2222);
    coverage_push(2);
    restore_checkpoint_ROB_LEN();
}

static void check_coverage(void) {
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    queue_fault_page = mmap(NULL, page_size, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(queue_fault_page != MAP_FAILED);
    setup_signal_handler();
    coverage_test = true;
    for (unsigned nested = 0; nested <= (MAX_CHECKPOINTS > 1); ++nested) {
        for (unsigned fault = 0; fault < 2; ++fault) {
            /* As the fuzzer leaves them: guard 3 has been retired. */
            for (unsigned i = 0; i < COVERAGE_GUARDS; ++i)
                __guard_start__teapot__[i] = i == 3 ? 0 : i + 1;
            coverage_nested = nested;
            queue_force_fault = fault;
            coverage_step = coverage_event_count = 0;
            coverage_word = 0x5555;
            checkpoint_cnt = 0;
            max_checkpoints = MAX_CHECKPOINTS;
            branch_counter = 0;
            memory_history_top = memory_history;
            guard_list_top = guard_list;
            assert(checkpoint_rollback_probe(stack_top) == 1);
            assert(checkpoint_cnt == 0 && coverage_word == 0x5555 && !in_restore_memlog);
            assert(memory_history_top == memory_history && guard_list_top == guard_list);
            /* Every pushed guard reached the fuzzer once, the inner window's
             * at the inner rollback, the rest newest first at the outer one. */
            static const unsigned flat[] = {0}, chain[] = {2, 1, 0};
            const unsigned *expected = nested ? chain : flat;
            unsigned count = nested ? 3 : 1;
            if (coverage_event_count != count) {
                fprintf(stderr, "nested=%u fault=%u: %u coverage events, expected %u\n",
                        nested, fault, coverage_event_count, count);
                abort();
            }
            for (unsigned i = 0; i < count; ++i)
                assert(coverage_events[i] == expected[i]);
        }
    }
    coverage_test = queue_force_fault = false;
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
    /* Exercise every width/alignment, and a fault halfway across a page. */
    for (unsigned offset = 0; offset < 8; ++offset) {
        for (unsigned width = 1; width <= 8; ++width) {
            memset(bytes, 0x55, sizeof(bytes));
            memory_history[0] = (memory_history_t){.addr = bytes + offset,
                .data = UINT64_C(0x8877665544332211), .size = width};
            memory_history_top = memory_history + 1;
            restore_checkpoint_memlog();
            for (unsigned i = 0; i < sizeof(bytes); ++i)
                assert(bytes[i] == (i >= offset && i < offset + width ?
                    ((const uint8_t *)&memory_history[0].data)[i-offset] : 0x55));
        }
    }
    unsigned char *pair = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(pair != MAP_FAILED);
    memset(pair, 0, page_size * 2);
    assert(mprotect(pair + page_size, page_size, PROT_READ) == 0);
    memory_history[0] = (memory_history_t){.addr = pair + page_size - 4,
                                          .data = UINT64_MAX, .size = 8};
    memory_history_top = memory_history + 1;
    if (sigsetjmp(memlog_fault_return, 1) == 0) {
        restore_checkpoint_memlog();
        abort();
    }
    for (unsigned i = 0; i < 4; ++i) {
        assert(pair[page_size - 4 + i] == 0xff);
        assert(pair[page_size + i] == 0);
    }
    restore_checkpoint_memlog();
    assert(munmap(pair, page_size * 2) == 0);
    assert(sigaction(SIGSEGV, &old_segv, NULL) == 0);
    assert(sigaction(SIGBUS, &old_bus, NULL) == 0);
    assert(munmap(page, page_size) == 0);
}

/* A rollback validates the whole log before it writes anything: an older
 * corrupt entry beneath a newer valid one stops it with the target untouched.
 * The rollback runs in a child; the target is shared memory the parent reads. */
static void check_memlog_validation(void) {
    uint64_t *target = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(target != MAP_FAILED);
    target[0] = UINT64_C(0x5555555555555555);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        checkpoint_cnt = 0;
        checkpoint_metadata[0].memory_history_top = memory_history;
        memory_history[0] = (memory_history_t){.addr = target + 1, .data = 0, .size = 0};  /* corrupt */
        memory_history[1] = (memory_history_t){.addr = target, .data = UINT64_MAX, .size = 8};
        memory_history_top = memory_history + 2;
        restore_checkpoint_memlog();
        _exit(0);  /* must not be reached */
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(target[0] == UINT64_C(0x5555555555555555));
    assert(munmap(target, 4096) == 0);
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

/* Replay runs from the newest entry to the oldest, so the oldest entry for a
 * byte wins, and it stops at the rolled-back checkpoint's lower bound. */
static void check_memlog_order(void) {
    static uint64_t word;
    unsigned char *bytes = (unsigned char *)&word;
    word = UINT64_C(0x5555555555555555);
    checkpoint_cnt = 0;
    checkpoint_metadata[0].memory_history_top = memory_history;
    memory_history[0] = (memory_history_t){.addr = bytes + 2, .data = UINT64_C(0x2222), .size = 2};
    memory_history[1] = (memory_history_t){.addr = bytes, .data = UINT64_C(0x1111111111111111), .size = 8};
    memory_history[2] = (memory_history_t){.addr = bytes + 4, .data = UINT64_C(0x33333333), .size = 4};
    memory_history_top = memory_history + 3;
    restore_checkpoint_memlog();
    static const unsigned char expected[8] = {0x11, 0x11, 0x22, 0x22, 0x11, 0x11, 0x11, 0x11};
    assert(memcmp(bytes, expected, sizeof(expected)) == 0);
    assert(memory_history_top == memory_history);
    for (int i = 0; i < 3; i++) assert(memory_history[i].size == 0);

    /* A nested rollback replays only the inner checkpoint's entries. */
    unsigned depth = MAX_CHECKPOINTS > 1 ? 1 : 0;
    word = UINT64_C(0x5555555555555555);
    checkpoint_cnt = depth;
    checkpoint_metadata[depth].memory_history_top = memory_history + 1;
    memory_history[0] = (memory_history_t){.addr = bytes, .data = 0, .size = 8};
    memory_history[1] = (memory_history_t){.addr = bytes + 4, .data = UINT64_C(0x44444444), .size = 4};
    memory_history_top = memory_history + 2;
    restore_checkpoint_memlog();
    assert(word == UINT64_C(0x4444444455555555));
    assert(memory_history_top == memory_history + 1);
    assert(memory_history[0].size == 8 && memory_history[1].size == 0);
    memory_history_top = memory_history;
    checkpoint_cnt = 0;
}

#if defined(__aarch64__) && defined(TEAPOT_AARCH64_MTE_TAG_STORAGE)
#ifndef PROT_MTE
#define PROT_MTE 0x20
#endif
static unsigned mte_tag_of(const void *granule) {
    uintptr_t tagged = (uintptr_t)granule;
    asm volatile("ldg %0, [%1]" : "+r"(tagged) : "r"(granule) : "memory");
    return (unsigned)(tagged >> 56) & 0xf;
}

static void mte_set_tag(void *granule, unsigned tag) {
    uintptr_t tagged = (uintptr_t)granule | ((uintptr_t)tag << 56);
    asm volatile("stg %0, [%1]" :: "r"(tagged), "r"(granule) : "memory");
}

/* A tag entry restores its granule's allocation tag, in log order with the
 * data entries around it: a granule logged twice ends with its oldest tag. */
static void check_memlog_mte(void) {
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *page = mmap(NULL, page_size, PROT_READ | PROT_WRITE | PROT_MTE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);
    uint64_t *data = (uint64_t *)(page + 16);
    mte_set_tag(page, 0x3);
    mte_set_tag(page + 16, 0x5);
    *data = UINT64_C(0x5555555555555555);
    checkpoint_cnt = 0;
    checkpoint_metadata[0].memory_history_top = memory_history;
    memory_history[0] = (memory_history_t){.addr = page, .data = 0x3, .size = MEM_HISTORY_MTE_TAG_SIZE};
    mte_set_tag(page, 0x9);
    memory_history[1] = (memory_history_t){.addr = data, .data = *data, .size = 8};
    *data = UINT64_MAX;
    memory_history[2] = (memory_history_t){.addr = page + 16, .data = 0x5, .size = MEM_HISTORY_MTE_TAG_SIZE};
    mte_set_tag(page + 16, 0xa);
    memory_history[3] = (memory_history_t){.addr = page, .data = 0x9, .size = MEM_HISTORY_MTE_TAG_SIZE};
    mte_set_tag(page, 0xc);
    memory_history_top = memory_history + 4;
    restore_checkpoint_memlog();
    assert(mte_tag_of(page) == 0x3);
    assert(mte_tag_of(page + 16) == 0x5);
    assert(*data == UINT64_C(0x5555555555555555));
    assert(memory_history_top == memory_history);
    for (int i = 0; i < 4; i++) assert(memory_history[i].size == 0);
    assert(munmap(page, page_size) == 0);
}
#endif

static void check_storage(void) {
    assert(memory_history_top == memory_history && guard_list_top == guard_list);
    const struct {
        void *address;
        size_t size;
        size_t alignment;
        bool zeroed;
    } objects[] = {
        {memory_history, sizeof(memory_history), 16, true},
        /* main() fills in the branch counter and fixed-register sources; see below. */
        {checkpoint_target_metadata, sizeof(checkpoint_target_metadata), 16, false},
        {guard_list, sizeof(guard_list), 16, true},
        {scratchpad, sizeof(scratchpad), 16, true},
        {&max_checkpoints, sizeof(max_checkpoints), 8, true},
    };
    uintptr_t start = (uintptr_t)__start_teapot_protected_bss;
    uintptr_t end = (uintptr_t)__stop_teapot_protected_bss;
    assert(end - start == sizeof(memory_history) + sizeof(checkpoint_target_metadata) +
           sizeof(guard_list) + sizeof(scratchpad) + sizeof(max_checkpoints));
    for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); i++) {
        uintptr_t address = (uintptr_t)objects[i].address;
        assert(start <= address && address + objects[i].size <= end);
        assert(address % objects[i].alignment == 0);
        if (!objects[i].zeroed)
            continue;
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
    for (size_t offset = 0; offset < CHECKPOINT_TARGET_METADATA_SIZE; offset += 8)
        if (offset != CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR &&
                offset != CHECKPOINT_TARGET_FIXED_REG0_SOURCE &&
                offset != CHECKPOINT_TARGET_FIXED_REG1_SOURCE)
            assert(checkpoint_target_metadata[offset / 8] == 0);
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
#if defined(__x86_64__)
    assert((uintptr_t)checkpoint_target_metadata % 16 == 0);
#endif
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
    else if (strcmp(argv[1], "memlog-validation") == 0)
        check_memlog_validation();
    else if (strcmp(argv[1], "memlog-order") == 0)
        check_memlog_order();
#if defined(__aarch64__) && defined(TEAPOT_AARCH64_MTE_TAG_STORAGE)
    else if (strcmp(argv[1], "memlog-mte") == 0)
        check_memlog_mte();
#endif
    else if (strcmp(argv[1], "queued-tags") == 0)
        check_queued_tags();
    else if (strcmp(argv[1], "coverage") == 0)
        check_coverage();
    else if (strcmp(argv[1], "assertions") == 0)
        check_assertions();
#if defined(__x86_64__)
    else if (strcmp(argv[1], "df") == 0)
        check_x64_df();
#endif
#if defined(__riscv)
    else if (strcmp(argv[1], "riscv64-float") == 0) {
        assert(check_riscv64_float != NULL);
        check_riscv64_float();
    }
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
    else if (strcmp(argv[1], "pac-fpac") == 0) {
        int status = check_aarch64_pac(stack_top);
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
