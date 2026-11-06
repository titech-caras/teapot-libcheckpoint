#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "checkpoint.h"
#include "nested_policy.h"
#include "nested_model.h"

extern uint64_t max_checkpoints;
extern uintptr_t checkpoint_target_metadata[CHECKPOINT_TARGET_METADATA_SIZE / 8];
extern memory_history_t *memory_history_top;
extern uint32_t *guard_list_top;
extern int checkpoint_entry_probe(void *stack);
extern int checkpoint_rollback_probe(void *stack);
extern size_t strlen__dift_wrapper__(const char *);
extern __attribute__((noreturn)) void restore_checkpoint_ROB_LEN(void);
uint32_t __guard_start__teapot__[1], __guard_end__teapot__[1];
/* As in the entry probes, test machinery without ASan instrumentation. */
void __asan_init(void) {}
void __asan_poison_memory_region(void const volatile *p, size_t n) { (void)p; (void)n; }
void __asan_unpoison_memory_region(void const volatile *p, size_t n) { (void)p; (void)n; }

static struct teapot_nested_site sites[3], reference[3];
static const uint64_t indices[] = {0, 17, UINT64_C(0xf1234567)};
static uint32_t generation;
static void *stack_top;
static unsigned steps, inner_rollbacks;
static uint64_t digest = UINT64_C(0xcbf29ce484222325), value;

static void metadata(unsigned site, unsigned outcome) {
    checkpoint_target_metadata[CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR / 8] = (uintptr_t)&sites[site];
    checkpoint_target_metadata[CHECKPOINT_TARGET_OUTCOME / 8] = outcome;
    checkpoint_target_metadata[CHECKPOINT_TARGET_SITE_INDEX / 8] = indices[site];
}

static void compare(void) {
    assert(teapot_nested_root_gen == generation);
    assert(memcmp(sites, reference, sizeof sites) == 0);
}

static int decision(unsigned site, unsigned outcome, unsigned depth) {
    metadata(site, outcome);
    checkpoint_cnt = depth;
    int expected = model_decision(&reference[site], indices[site], outcome, depth,
                                  &generation, teapot_nested_seed_word);
    int actual = checkpoint_entry_probe(stack_top);
    if (actual != expected) {
        fprintf(stderr, "decision %u: site=%u outcome=%u depth=%u root=%" PRIu32
                " count=%u expected=%d actual=%d\n", steps, site, outcome, depth,
                generation, depth ? reference[site].tree[outcome] : reference[site].arch[outcome],
                expected, actual);
        abort();
    }
    assert(checkpoint_cnt == depth + actual);
    if (!depth && actual)
        assert(max_checkpoints == 6);
    compare();
    assert(memory_history_top == memory_history);
    digest = (digest ^ (site * 32 + outcome * 16 + depth * 2 + actual)) * UINT64_C(0x100000001b3);
    ++steps;
    checkpoint_cnt = 0;
    return actual;
}

static void logged_write(uint64_t next) {
    *memory_history_top++ = (memory_history_t){.addr = &value, .data = value, .size = 8};
    value = next;
}

/* Entered by the real rollback probe, then again by its nested child. */
__attribute__((noreturn)) void checkpoint_test_transient_body(void) {
    if (checkpoint_cnt == 2) {
        assert(memory_history_top == memory_history + 1 && value == 11);
        ++inner_rollbacks;
        logged_write(99);
        restore_checkpoint_ROB_LEN();
    }
    assert(checkpoint_cnt == 1 && memory_history_top == memory_history && value == 7);
    logged_write(11);
    for (unsigned i = 0; i < 32; ++i) {
        metadata(1, i & 1);
        int expected = model_decision(&reference[1], indices[1], i & 1, 1,
                                      &generation, teapot_nested_seed_word);
        unsigned before = inner_rollbacks;
        checkpoint_rollback_probe((char *)stack_top - 65536);
        assert(inner_rollbacks == before + expected);
        compare();
        assert(checkpoint_cnt == 1 && value == 11 && memory_history_top == memory_history + 1);
        /* All entries are application data; tree/gen never enter the log. */
        assert(memory_history[0].addr == &value);
    }
    assert(inner_rollbacks > 1 && sites[1].tree[0] == 16 && sites[1].tree[1] == 16);
    restore_checkpoint_ROB_LEN();
}

int main(int argc, char **argv) {
    libcheckpoint_enable(argc, argv);
    /* The new archive must carry the usual libc bridges as well. */
    assert(strlen__dift_wrapper__("hashed") == 6);
    const char *pipe_data = getenv("TEAPOT_TEST_PIPE");
    if (pipe_data) {
        int fd = open(argv[1], O_RDONLY);
        char buffer[128];
        ssize_t n = read(fd, buffer, sizeof buffer);
        assert(n == (ssize_t)strlen(pipe_data) && memcmp(buffer, pipe_data, n) == 0);
        assert(read(fd, buffer, sizeof buffer) == 0);
        close(fd);
    }
    const char *expected_seed = getenv("TEAPOT_TEST_SEED");
    if (expected_seed) {
        assert(teapot_nested_seed_word == strtoull(expected_seed, NULL, 16));
        /* Enabling a second time does not recompute the input/override seed. */
        setenv("TEAPOT_NESTED_SEED", "0", 1);
        libcheckpoint_disable();
        libcheckpoint_enable(0, NULL);
        assert(teapot_nested_seed_word == strtoull(expected_seed, NULL, 16));
        printf("seed=%016" PRIx64 "\n", teapot_nested_seed_word);
        return 0;
    }
    size_t stack_size = 3 * AARCH64_SHADOW_STACK_SIZE;
    void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(stack != MAP_FAILED);
    stack_top = (char *)stack + stack_size - 4096;
    checkpoint_target_metadata[CHECKPOINT_TARGET_FIXED_REG0_SOURCE / 8] = UINTPTR_MAX;
    checkpoint_target_metadata[CHECKPOINT_TARGET_FIXED_REG1_SOURCE / 8] = UINTPTR_MAX;
    memory_history_top = memory_history;
    guard_list_top = guard_list;
    /* Exhaust the two independent root counters, including saturation. */
    for (unsigned i = 0; i < 0x10002; ++i)
        for (unsigned o = 0; o < 2; ++o)
            decision(0, o, 0);
    assert(generation == 32);
    /* Several roots, all octave positions through e=10, both outcomes and
     * multiple site indices, interleaved across all transient depths. */
    for (unsigned root = 0; root < 5; ++root) {
        while (!decision(2, root & 1, 0)) {}
        for (unsigned t = 0; t < 2048; ++t)
            for (unsigned o = 0; o < 2; ++o)
                decision(t & 1, o, 1 + t % 5);
    }
    /* Saturation and the depth cap (which does not even reset a generation). */
    for (unsigned t = 0; t < 0x10000; ++t)
        for (unsigned o = 0; o < 2; ++o)
            decision(1, o, 1 + t % 5);
    sites[1].tree[0] = reference[1].tree[0] = 0xfffe;
    decision(1, 0, 1);
    decision(1, 0, 1);
    sites[1].gen = reference[1].gen = 0;
    decision(1, 1, 6);
    decision(1, 1, 1);
    assert(sites[1].tree[0] == 0 && sites[1].tree[1] == 1);
    /* Generation wrapping uses u32 and skips zero. */
    teapot_nested_root_gen = generation = UINT32_MAX;
    sites[2].arch[0] = reference[2].arch[0] = 0;
    decision(2, 0, 0);
    assert(generation == 1);
    /* Deterministic mixed trace makes every decision observable. */
    uint32_t random = 0x12345678;
    for (unsigned i = 0; i < 8192; ++i) {
        random = random * 1664525u + 1013904223u;
        decision(random % 3, (random >> 8) & 1, (random >> 16) % 7);
    }
    memset(sites, 0, sizeof sites);
    memset(reference, 0, sizeof reference);
    teapot_nested_root_gen = generation = 0;
    value = 7;
    metadata(0, 1);
    assert(model_decision(&reference[0], indices[0], 1, 0, &generation, teapot_nested_seed_word));
    checkpoint_rollback_probe(stack_top);
    compare();
    assert(checkpoint_cnt == 0 && value == 7 && memory_history_top == memory_history);
    assert(sites[1].tree[0] == 16 && sites[1].tree[1] == 16);
    printf("%u decisions matched; trace=%016" PRIx64 "; %u nested rollbacks; tree/gen persisted\n",
           steps, digest, inner_rollbacks);
    assert(munmap(stack, stack_size) == 0);
    return 0;
}
