#define _GNU_SOURCE
#include "fault_sites.h"
#include "runtime_contract.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

extern const struct teapot_fault_site_table fault_test_table;
extern const struct teapot_fault_site_table fault_one_table, fault_max_table, fault_zero_table;
extern const char fault_pc_threshold1[], fault_pc_threshold255[], fault_pc_threshold0[];
extern const char fault_pc_first[], fault_pc_last[], fault_text_start[], fault_text_end[];
extern const char fault_copy_first[], fault_copy_last[];
extern const char __start_teapot_protected_bss[], __stop_teapot_protected_bss[];
static const struct libcheckpoint_contract_record records[] = {
    { .magic = LIBCHECKPOINT_CONTRACT_MAGIC, .version = LIBCHECKPOINT_CONTRACT_VERSION,
      .kind = LIBCHECKPOINT_CONTRACT_KIND_MODULE, .header_size = LIBCHECKPOINT_CONTRACT_HEADER_SIZE,
      .capabilities = LIBCHECKPOINT_CAPABILITY_FAULT_TRAINING, .fault_sites = &fault_zero_table },
    { .magic = LIBCHECKPOINT_CONTRACT_MAGIC, .version = LIBCHECKPOINT_CONTRACT_VERSION,
      .kind = LIBCHECKPOINT_CONTRACT_KIND_MODULE, .header_size = LIBCHECKPOINT_CONTRACT_HEADER_SIZE,
      .capabilities = LIBCHECKPOINT_CAPABILITY_FAULT_TRAINING, .fault_sites = &fault_max_table },
    { .magic = LIBCHECKPOINT_CONTRACT_MAGIC, .version = LIBCHECKPOINT_CONTRACT_VERSION,
      .kind = LIBCHECKPOINT_CONTRACT_KIND_MODULE, .header_size = LIBCHECKPOINT_CONTRACT_HEADER_SIZE,
      .capabilities = LIBCHECKPOINT_CAPABILITY_FAULT_TRAINING, .fault_sites = &fault_one_table },
    { .magic = LIBCHECKPOINT_CONTRACT_MAGIC, .version = LIBCHECKPOINT_CONTRACT_VERSION,
      .kind = LIBCHECKPOINT_CONTRACT_KIND_MODULE, .header_size = LIBCHECKPOINT_CONTRACT_HEADER_SIZE,
      .capabilities = LIBCHECKPOINT_CAPABILITY_FAULT_TRAINING, .fault_sites = &fault_test_table }
};
#define RECORDS_END (records + sizeof(records) / sizeof(records[0]))
#define TEST_TABLE_BYTES (TEAPOT_FAULT_SITE_HEADER_SIZE + 2 * TEAPOT_FAULT_SITE_ENTRY_SIZE)
#if defined(__x86_64__)
#define ACCESS_LENGTH 7
#else
#define ACCESS_LENGTH 4
#endif

#ifdef TEST_FAULT_STARTUP
/* Every call is monitored, even free(NULL) or qsort on a tiny table. stdio
 * and libc sorting may allocate internally without calling a link-wrapped
 * malloc, so monitor those entry points too. This catches the real old
 * collector, not just a synthetic allocator model. */
static volatile unsigned watched_calls;
static volatile bool watching_startup;
#define WATCH() do { if (watching_startup) ++watched_calls; } while (0)
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void *__real_realloc(void *, size_t);
extern void __real_free(void *);
extern FILE *__real_fopen(const char *, const char *);
extern FILE *__real_fdopen(int, const char *);
extern ssize_t __real_getline(char **, size_t *, FILE *);
extern void __real_qsort(void *, size_t, size_t, int (*)(const void *, const void *));
extern void *__real_mmap(void *, size_t, int, int, int, off_t);
void *__wrap_malloc(size_t n) { WATCH(); return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t s) { WATCH(); return __real_calloc(n, s); }
void *__wrap_realloc(void *p, size_t n) { WATCH(); return __real_realloc(p, n); }
void __wrap_free(void *p) { WATCH(); __real_free(p); }
FILE *__wrap_fopen(const char *p, const char *m) { WATCH(); return __real_fopen(p, m); }
FILE *__wrap_fdopen(int fd, const char *m) { WATCH(); return __real_fdopen(fd, m); }
ssize_t __wrap_getline(char **p, size_t *n, FILE *f) { WATCH(); return __real_getline(p, n, f); }
void __wrap_qsort(void *p, size_t n, size_t s, int (*cmp)(const void *, const void *)) {
    WATCH(); __real_qsort(p, n, s, cmp);
}
void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t offset) {
    WATCH(); return __real_mmap(p, n, prot, flags, fd, offset);
}

static void hardware_sample(uintptr_t address, unsigned char sample[64]) {
    /* Observe raw readable bytes, as speculation does, without asking the C
     * optimizer to dereference a freed object. The test is unsanitized and the
     * allocator's freed 1024-byte chunk remains mapped in this single thread.
     * Byte assembly also works under QEMU without process_vm_readv emulation. */
    for (size_t i = 0; i < 64; ++i) {
        unsigned value;
#if defined(__x86_64__)
        __asm__ volatile ("movzbl (%1),%0" : "=r"(value) : "r"(address + i) : "memory");
#elif defined(__aarch64__)
        __asm__ volatile ("ldrb %w0,[%1]" : "=r"(value) : "r"(address + i) : "memory");
#else
        __asm__ volatile ("lbu %0,0(%1)" : "=r"(value) : "r"(address + i) : "memory");
#endif
        sample[i] = (unsigned char)value;
    }
}
static void startup_probe(bool off) {
    assert(setenv("TEAPOT_FAULT_ADAPTATION", off ? "0" : "1", 1) == 0);
    unsigned char *chunk = malloc(1024); assert(chunk);
    memset(chunk, 0x53, 1024);
    uintptr_t address = (uintptr_t)chunk;
    free(chunk);
    unsigned char before[64], after[64];
    hardware_sample(address, before);
    watching_startup = true;
    teapot_fault_registry_initialize(records, RECORDS_END);
    struct teapot_fault_low_policy policy = teapot_fault_verify_low(65536, !off);
    watching_startup = false;
    hardware_sample(address, after);
    /* Print the residue-dependent probe before assertions so a failing old
     * collector has an inspectable observation, not just an abort signal. */
    printf("startup allocation calls: %u; freed-memory report parity: %s\n",
           watched_calls, memcmp(before, after, sizeof(before)) ? "FAIL" : "PASS");
    fflush(stdout);
    assert(watched_calls == 0);
    assert(memcmp(before, after, sizeof(before)) == 0);
    size_t module, site;
    assert(teapot_fault_lookup((uintptr_t)fault_copy_first, &module, &site) && module == 0 && site == 0);
    if (off) assert(!policy.bound && (policy.failures & TEAPOT_FAULT_LOW_DISABLED));
}
#endif

static void relative64(int64_t *field, uintptr_t target) {
    uintptr_t anchor = (uintptr_t)field;
    *field = target >= anchor ? (int64_t)(target - anchor) : -(int64_t)(anchor - target);
}

static void malformed_helpers(void) {
    /* Synthetic local address ranges let these pure checks work on every ISA
     * without depending on the stack-to-text distance. No code is executed. */
    struct fake {
        struct teapot_fault_site_table header;
        struct teapot_fault_site_entry entries[2];
        uint32_t counters[2], pending[2];
    } f = {0};
    struct teapot_fault_site_table *t = &f.header;
    t->magic = TEAPOT_FAULT_SITE_MAGIC; t->version = TEAPOT_FAULT_SITE_VERSION; t->header_size = 112;
    t->entry_size = TEAPOT_FAULT_SITE_ENTRY_SIZE; t->count = 2; t->flags = 1; t->threshold = 2;
    uintptr_t text = (uintptr_t)&f + 256;
    relative64(&t->text_start, text); relative64(&t->text_end, text + 64);
    relative64(&t->stub_start, text); relative64(&t->stub_end, text + 64);
    relative64(&t->counters_start, (uintptr_t)f.counters);
    relative64(&t->counters_end, (uintptr_t)(f.counters + 2));
    relative64(&t->pending_start, (uintptr_t)f.pending);
    relative64(&t->pending_end, (uintptr_t)(f.pending + 2));
    for (size_t i = 0; i < 2; ++i) {
        f.entries[i].fault_pc = (int32_t)(text + i * 8 - (uintptr_t)&f.entries[i].fault_pc);
        f.entries[i].stub = (int32_t)(text + 32 + i * 16 - (uintptr_t)&f.entries[i].stub);
        f.entries[i].copy_pc = (int32_t)(text + 32 + i * 16 - (uintptr_t)&f.entries[i].copy_pc);
        f.entries[i].length = ACCESS_LENGTH;
    }
    const uintptr_t begin = (uintptr_t)f.counters, end = (uintptr_t)(f.pending + 2);
#define VALIDATE() teapot_fault_validate_table(t, TEST_TABLE_BYTES, begin, end)
    assert(VALIDATE());
    t->threshold = 256; assert(!VALIDATE()); t->threshold = 2;
    t->reserved[0] = 1; assert(!VALIDATE()); t->reserved[0] = 0;
    t->count = 3; assert(!VALIDATE()); t->count = 2;
    t->flags = 0; assert(!VALIDATE()); t->flags = 1;
    t->entry_size = 8; assert(!VALIDATE()); t->entry_size = TEAPOT_FAULT_SITE_ENTRY_SIZE;
    t->version = 1; assert(!VALIDATE()); t->version = TEAPOT_FAULT_SITE_VERSION;
    f.entries[0].length = 2; assert(!VALIDATE()); f.entries[0].length = ACCESS_LENGTH;
    f.entries[0].flags = 1; assert(!VALIDATE()); f.entries[0].flags = 0;
    f.entries[1].copy_pc += 64; assert(!VALIDATE()); f.entries[1].copy_pc -= 64;
#if !defined(__x86_64__)
    f.entries[0].copy_pc += 2; assert(!VALIDATE()); f.entries[0].copy_pc -= 2;
#endif
    int32_t original = f.entries[1].fault_pc;
    f.entries[1].fault_pc -= 8; assert(!VALIDATE()); /* duplicate */
    f.entries[1].fault_pc -= 8; assert(!VALIDATE()); /* unsorted */
    f.entries[1].fault_pc = original;
    int64_t saved = t->pending_start;
    relative64(&t->pending_start, (uintptr_t)f.counters); assert(!VALIDATE());
    t->pending_start = saved;
    saved = t->text_start; t->text_start = INT64_MIN; assert(!VALIDATE()); t->text_start = saved;
    assert(VALIDATE());
#undef VALIDATE
}

static void helpers(void) {
    malformed_helpers();
    uintptr_t result; size_t site;
    assert(teapot_fault_resolve_relative(10, -10, &result) && result == 0);
    assert(teapot_fault_resolve_relative(UINTPTR_MAX, 0, &result) && result == UINTPTR_MAX);
    assert(!teapot_fault_resolve_relative(0, -1, &result));
    assert(!teapot_fault_resolve_relative(UINTPTR_MAX, 1, &result));
    assert(!teapot_fault_resolve_relative(0, INT64_MIN, &result));
    assert(teapot_fault_resolve_relative(UINT64_C(1) << 63, INT64_MIN, &result) && !result);
    assert(teapot_fault_validate_table(&fault_test_table, TEST_TABLE_BYTES, (uintptr_t)__start_teapot_protected_bss,
                                      (uintptr_t)__stop_teapot_protected_bss));
    assert(teapot_fault_table_lookup(&fault_test_table, (uintptr_t)fault_pc_first, &site) && site == 0);
    assert(teapot_fault_table_lookup(&fault_test_table, (uintptr_t)fault_pc_last, &site) && site == 1);
    assert(!teapot_fault_table_lookup(&fault_test_table, (uintptr_t)fault_pc_first + 1, &site));
    assert(!teapot_fault_table_lookup(&fault_test_table, (uintptr_t)fault_text_end, &site));
    uint32_t words[2] = {0}, pending[2] = {0};
    for (unsigned i = 0; i < 300; ++i) assert(teapot_fault_increment(words, 1) == (i < 255 ? i + 1 : 255));
    assert(words[0] == 0xff00 && !words[1]);
    assert(teapot_fault_increment(words, 3) == 1 && teapot_fault_increment(words, 4) == 1);
    for (unsigned threshold = 0; threshold <= 255; ++threshold) {
        uint32_t c = 0, p = 0;
        for (unsigned i = 0; i < 300; ++i) {
            uint8_t value = teapot_fault_increment(&c, 0);
            if (threshold && value >= threshold) teapot_fault_mark_pending(&p, 0);
            assert((p == 1) == (threshold && i + 1 >= threshold));
        }
        assert(teapot_fault_claim_pending(&p, 0) == (threshold != 0));
        assert(!teapot_fault_claim_pending(&p, 0));
        if (threshold) assert(!teapot_fault_mark_pending(&p, 0));
    }
    assert(teapot_fault_mark_pending(pending, 2));
    assert(!teapot_fault_mark_pending(pending, 2));
    assert(teapot_fault_claim_pending(pending, 2));
    assert(!teapot_fault_mark_pending(pending, 2));
    struct teapot_fault_low_facts f = { .known = true, .enabled = true, .mmap_min_addr = 65536 };
    assert(teapot_fault_low_policy(&f, 65536).bound == 65536);
    for (unsigned condition = 0; condition < 6; ++condition) {
        struct teapot_fault_low_facts bad = f;
        switch (condition) {
        case 0: bad.known = false; break;
        case 1: bad.enabled = false; break;
        case 2: bad.mmap_min_addr = 65535; break;
        case 3: bad.cap_sys_rawio = true; break;
        case 4: bad.mmap_page_zero = true; break;
        case 5: bad.mapped_below_bound = true; break;
        }
        assert(teapot_fault_low_policy(&bad, 65536).bound == 0);
        assert(teapot_fault_low_policy(&bad, 65536).failures != 0);
    }
    assert(!teapot_fault_low_policy(NULL, 65536).bound);
    assert(!teapot_fault_verify_low(65536, false).bound);
    /* Collector succeeds conservatively, never assumes the host is eligible. */
    struct teapot_fault_low_policy actual = teapot_fault_verify_low(65536, true);
    assert((actual.bound == 65536 && !actual.failures) || (!actual.bound && actual.failures));
}

static volatile sig_atomic_t nesting;
static void nested_handler(int sig) {
    assert(sig == SIGUSR1);
    siginfo_t info = { .si_code = SEGV_MAPERR };
    assert(teapot_fault_train(SIGSEGV, &info, (uintptr_t)fault_pc_last, true, false));
    if (!nesting) { nesting = 1; assert(raise(SIGUSR1) == 0); }
}

static void nested_signals(void) {
    assert(setenv("TEAPOT_FAULT_ADAPTATION", "1", 1) == 0);
    teapot_fault_registry_initialize(records, RECORDS_END);
    struct sigaction action = { .sa_handler = nested_handler, .sa_flags = SA_NODEFER };
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    assert(raise(SIGUSR1) == 0);
    assert(teapot_fault_counter(0, 1) == 2);
    struct teapot_fault_event event;
    assert(teapot_fault_next_pending(&event) && event.site == 1 && event.count == 2);
    assert(!teapot_fault_next_pending(&event));
}

static void registry_test(bool off) {
    assert(setenv("TEAPOT_FAULT_ADAPTATION", off ? "0" : "1", 1) == 0);
    teapot_fault_registry_initialize(records, RECORDS_END);
    teapot_fault_registry_initialize(records, RECORDS_END); /* idempotent */
    size_t module, site;
    assert(teapot_fault_lookup((uintptr_t)fault_pc_first, &module, &site) && module == 0 && site == 0);
    assert(teapot_fault_lookup((uintptr_t)fault_pc_last, &module, &site) && site == 1);
    assert(teapot_fault_lookup((uintptr_t)fault_copy_first, &module, &site) && module == 0 && site == 0);
    assert(teapot_fault_lookup((uintptr_t)fault_copy_last, &module, &site) && module == 0 && site == 1);
    assert(!teapot_fault_lookup((uintptr_t)fault_copy_first + 1, &module, &site));
    assert(!teapot_fault_lookup((uintptr_t)fault_text_start - 1, &module, &site));
    assert(!teapot_fault_lookup((uintptr_t)fault_text_end - 1, &module, &site));
    assert(teapot_fault_lookup((uintptr_t)fault_pc_threshold1, &module, &site) && module == 1 && site == 0);
    siginfo_t info = { .si_code = SEGV_MAPERR };
    int codes[] = { SI_USER, SI_TKILL, SI_QUEUE, SI_KERNEL, 8, 9, 10 };
    for (size_t i = 0; i < sizeof(codes) / sizeof(*codes); ++i) {
        info.si_code = codes[i];
        assert(!teapot_fault_train(SIGSEGV, &info, (uintptr_t)fault_pc_first, true, false));
    }
    info.si_code = SEGV_MAPERR;
    assert(!teapot_fault_train(SIGILL, &info, (uintptr_t)fault_pc_first, true, false));
    assert(!teapot_fault_train(SIGSEGV, &info, (uintptr_t)fault_pc_first, false, false));
    assert(!teapot_fault_train(SIGSEGV, &info, (uintptr_t)fault_pc_first, true, true));
    assert(!teapot_fault_train(SIGSEGV, &info, (uintptr_t)fault_pc_first + 1, true, false));
    assert(teapot_fault_counter(0, 0) == 0);
    struct teapot_fault_event event;
    for (int i = 0; i < 300; ++i) {
        assert(teapot_fault_train(SIGSEGV, &info, (uintptr_t)fault_pc_first, true, false) == !off);
        if (i == 0) assert(!teapot_fault_next_pending(&event));
    }
    assert(teapot_fault_counter(0, 0) == (off ? 0 : 255));
    assert(teapot_fault_next_pending(&event) == !off);
    if (!off) assert(event.site == 0 && event.count == 255 && event.fault_pc == (uintptr_t)fault_pc_first &&
                     event.copy_pc == (uintptr_t)fault_copy_first && event.stub == (uintptr_t)fault_copy_first &&
                     event.length == ACCESS_LENGTH);
    assert(!teapot_fault_next_pending(&event));
    assert(teapot_fault_train(SIGSEGV, &info, (uintptr_t)fault_pc_first, true, false) == !off);
    assert(!teapot_fault_next_pending(&event));
    info.si_code = SEGV_ACCERR;
    assert(teapot_fault_train(SIGSEGV, &info, (uintptr_t)fault_copy_last, true, false) == !off);
    assert(teapot_fault_counter(0, 1) == (off ? 0 : 1));
    assert(!teapot_fault_next_pending(&event));
    assert(teapot_fault_train(SIGSEGV, &info, (uintptr_t)fault_pc_last, true, false) == !off);
    assert(teapot_fault_next_pending(&event) == !off);
    if (!off) assert(event.site == 1 && event.count == 2);
    assert(teapot_fault_train(SIGSEGV, &info, (uintptr_t)fault_pc_threshold1, true, false) == !off);
    assert(teapot_fault_next_pending(&event) == !off);
    if (!off) assert(event.module == 1 && event.count == 1);
    for (int i = 0; i < 300; ++i) {
        assert(teapot_fault_train(SIGSEGV, &info, (uintptr_t)fault_pc_threshold255, true, false) == !off);
        assert(!teapot_fault_train(SIGSEGV, &info, (uintptr_t)fault_pc_threshold0, true, false));
        if (i < 254) assert(!teapot_fault_next_pending(&event));
    }
    assert(teapot_fault_counter(2, 0) == (off ? 0 : 255));
    assert(teapot_fault_counter(3, 0) == 0);
    assert(teapot_fault_next_pending(&event) == !off);
    if (!off) assert(event.module == 2 && event.count == 255);
    assert(!teapot_fault_next_pending(&event));
}

static void malformed(void) {
    /* Copy and rebase every self-relative field to preserve good metadata,
     * then mutate one invariant at a time. Startup must reject writable tables. */
    _Alignas(8) unsigned char data[TEST_TABLE_BYTES];
    memcpy(data, &fault_test_table, sizeof(data));
    struct teapot_fault_site_table *t = (void *)data;
    for (size_t offset = 24; offset < 88; offset += 8) {
        int64_t *field = (void *)(data + offset);
        *field += (uintptr_t)&fault_test_table - (uintptr_t)data;
    }
    /* Entries cannot be rebased onto a distant stack in int32. For pure
     * malformed checks all targets are local to this fake table. */
    t->text_start = 0; t->text_end = 256;
    t->stub_start = -16; t->stub_end = 240;
    t->counters_start = 0; t->counters_end = 0;
    assert(!teapot_fault_validate_table(t, sizeof(data), (uintptr_t)data, (uintptr_t)data + sizeof(data)));
    for (size_t bytes = 0; bytes < sizeof(*t); ++bytes)
        assert(!teapot_fault_validate_table(t, bytes, 0, UINTPTR_MAX));
    t->magic = 0; assert(!teapot_fault_validate_table(t, sizeof(data), 0, UINTPTR_MAX));
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        struct libcheckpoint_contract_record bad = records[3]; bad.fault_sites = t;
        teapot_fault_registry_initialize(&bad, &bad + 1); _exit(99);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "helpers")) helpers();
    else if (!strcmp(argv[1], "registry")) registry_test(false);
    else if (!strcmp(argv[1], "off")) registry_test(true);
    else if (!strcmp(argv[1], "malformed")) malformed();
    else if (!strcmp(argv[1], "nested")) nested_signals();
#ifdef TEST_FAULT_STARTUP
    else if (!strcmp(argv[1], "startup-on")) startup_probe(false);
    else if (!strcmp(argv[1], "startup-off")) startup_probe(true);
#endif
    else if (!strcmp(argv[1], "bench")) {
        teapot_fault_registry_initialize(records, RECORDS_END);
        struct timespec start, end; size_t module, site, hits = 0;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (size_t i = 0; i < 1000000; ++i) hits += teapot_fault_lookup((uintptr_t)fault_pc_first, &module, &site);
        clock_gettime(CLOCK_MONOTONIC, &end);
        printf("lookup: %zu hits, %.2f ns/lookup (four-module, five-site registry)\n", hits,
               (end.tv_sec - start.tv_sec) * 1e3 + (end.tv_nsec - start.tv_nsec) / 1e6);
        assert(hits == 1000000);
        size_t count = 1000000;
        struct teapot_fault_site_table *large = calloc(1, sizeof(*large) + count * sizeof(large->entries[0]));
        assert(large); large->count = count;
        uintptr_t base = (uintptr_t)large + 16 * count;
        for (size_t i = 0; i < count; ++i)
            large->entries[i].fault_pc = (int32_t)(base + 4 * i - (uintptr_t)&large->entries[i].fault_pc);
        hits = 0;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (size_t i = 0; i < 100000; ++i)
            hits += teapot_fault_table_lookup(large, base + 4 * ((i * 48271) % count), &site);
        clock_gettime(CLOCK_MONOTONIC, &end);
        printf("lookup: %zu hits, %.2f ns/lookup (million-entry synthetic table, permuted PCs)\n", hits,
               (end.tv_sec - start.tv_sec) * 1e4 + (end.tv_nsec - start.tv_nsec) / 1e5);
        assert(hits == 100000); free(large);
    } else assert(false);
    puts("fault infrastructure passed");
}
