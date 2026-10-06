/* White-box tests keep parsing/sorting private to the runtime rather than
 * exposing a test-only public ABI. This target compiles that source once. */
#include "../src/fault_sites.c"
#include <assert.h>
#include <sys/wait.h>

extern const struct teapot_fault_site_table fault_test_table;

static void parser(void) {
    struct mapping m;
    const char *good[] = {
        "0000000000001000-0000000000002000 r-xp 00000000 00:00 0",
        "1000-2000 rw-s 0 00:00 0",
        "FFFFFFFFFFFFF000-FFFFFFFFFFFFFFFF ---p 0 00:00 0",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i)
        assert(parse_mapping(good[i], strlen(good[i]), &m));
    const char *bad[] = {
        "", "-2000 r-xp ", "1000- r-xp ", "2000-1000 r-xp ",
        "1000-1000 r-xp ", "1000-2000 r-xz ", "1000-2000 rwyp ",
        "1000-2000 r-xp", "10000000000000000-2000 r-xp ",
        "1000-2000 r-xpX", "1000/2000 r-xp ",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(!parse_mapping(bad[i], strlen(bad[i]), &m));
    struct fake {
        struct teapot_fault_site_table header;
        struct teapot_fault_site_entry entries[7];
    } f = {0};
    f.header.count = 7;
    f.header.entry_size = TEAPOT_FAULT_SITE_ENTRY_SIZE;
    const unsigned order[] = {6, 0, 4, 2, 5, 1, 3};
    uint32_t indexes[7];
    for (size_t i = 0; i < 7; ++i) {
        indexes[i] = (uint32_t)i;
        uintptr_t target = (uintptr_t)&f + 1024 + order[i] * 16;
        f.entries[i].copy_pc = (int32_t)(target - (uintptr_t)&f.entries[i].copy_pc);
    }
    sort_copies(&f.header, indexes);
    for (size_t i = 0; i < 7; ++i) assert(order[indexes[i]] == i);
    uintptr_t pool = (uintptr_t)fault_copy_pool;
    assert(registry_storage_overlap(pool, pool + 8));
    assert(registry_storage_overlap(pool - 8, pool + 8));
    /* The registry pool immediately precedes the copy pool: its last eight
     * bytes are also reserved. The true outside boundary is after both. */
    assert(!registry_storage_overlap(pool + sizeof(fault_copy_pool), pool + sizeof(fault_copy_pool) + 8));
    pool = (uintptr_t)&fault_registry_pool;
    assert(registry_storage_overlap(pool, pool + 8));
}

static size_t merged(const struct mapping *maps, size_t count, size_t i, uintptr_t *end) {
    *end = maps[i++].end;
    while (i < count && maps[i].start == *end) *end = maps[i++].end;
    return i;
}

__attribute__((noinline)) static void prime_stack(void) {
    /* The test's large before[] frame reaches the end of the initial stack
     * VMA. Commit headroom before the snapshot so ordinary deeper C frames do
     * not masquerade as a runtime-created mapping. Heap and mmap coverage are
     * still compared exactly; no region is excluded from the assertion. */
    volatile unsigned char headroom[65536];
    for (size_t i = 0; i < sizeof(headroom); i += 4096) headroom[i] = 0;
    headroom[sizeof(headroom) - 1] = 0;
}

static void no_new_mappings(void) {
    struct mapping before[FAULT_MAP_LIMIT]; size_t n, after_n;
    prime_stack();
    assert(read_mappings(&n));
    memcpy(before, startup_maps, n * sizeof(*before));
    const struct libcheckpoint_contract_record r = {
        .magic = LIBCHECKPOINT_CONTRACT_MAGIC, .version = LIBCHECKPOINT_CONTRACT_VERSION,
        .kind = LIBCHECKPOINT_CONTRACT_KIND_MODULE,
        .capabilities = LIBCHECKPOINT_CAPABILITY_FAULT_TRAINING,
        .header_size = LIBCHECKPOINT_CONTRACT_HEADER_SIZE, .fault_sites = &fault_test_table,
    };
    teapot_fault_registry_initialize(&r, &r + 1);
    assert(read_mappings(&after_n));
    /* Protecting our existing pages may split VMAs but cannot add, remove or
     * move any mapped address. Compare merged coverage, not line counts. */
    size_t a = 0, b = 0;
    while (a < n && b < after_n) {
        if (before[a].start != startup_maps[b].start) {
            fprintf(stderr, "mapping coverage differs: before[%zu]=%#lx-%#lx, after[%zu]=%#lx-%#lx\n",
                    a, (unsigned long)before[a].start, (unsigned long)before[a].end,
                    b, (unsigned long)startup_maps[b].start, (unsigned long)startup_maps[b].end);
        }
        assert(before[a].start == startup_maps[b].start);
        uintptr_t ae, be;
        a = merged(before, n, a, &ae); b = merged(startup_maps, after_n, b, &be);
        assert(ae == be);
    }
    assert(a == n && b == after_n);
    for (unsigned which = 0; which < 2; ++which) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            if (which) ((volatile uint32_t *)fault_copy_pool)[0] ^= 1;
            else ((volatile unsigned char *)&fault_registry_pool)[0] ^= 1;
            _exit(99);
        }
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    }
}

static void startup_storage(void) {
    const uintptr_t begin = (uintptr_t)__start_teapot_protected_bss;
    const uintptr_t end = (uintptr_t)__stop_teapot_protected_bss;
    const uintptr_t maps = (uintptr_t)startup_maps, buffer = (uintptr_t)proc_buffer;
    assert(begin && end > begin);
    assert(maps % 8 == 0 && buffer % 8 == 0);
    assert(maps >= begin && maps + sizeof(startup_maps) <= end);
    assert(buffer >= begin && buffer + sizeof(proc_buffer) <= end);
    assert(buffer == maps + sizeof(startup_maps));
    /* Check kernel-zeroed bytes before any startup collector uses them. */
    for (size_t i = 0; i < sizeof(startup_maps); ++i)
        assert(((volatile unsigned char *)startup_maps)[i] == 0);
    for (size_t i = 0; i < sizeof(proc_buffer); ++i)
        assert(((volatile unsigned char *)proc_buffer)[i] == 0);
    assert(!registry_storage_overlap(maps, maps + sizeof(startup_maps)));
    assert(!registry_storage_overlap(buffer, buffer + sizeof(proc_buffer)));
    assert(buffer + sizeof(proc_buffer) <= (uintptr_t)&fault_registry_pool);
    assert((uintptr_t)&fault_registry_pool % 65536 == 0);
    assert((uintptr_t)fault_copy_pool % 65536 == 0);
    /* Initializes and makes both pool ranges read-only; also rereads maps
     * through this mutable scratch and checks the immutable ranges fault. */
    no_new_mappings();
    ((volatile unsigned char *)startup_maps)[0] = 0x12;
    ((volatile unsigned char *)startup_maps)[sizeof(startup_maps) - 1] = 0x34;
    ((volatile unsigned char *)proc_buffer)[0] = 0x56;
    ((volatile unsigned char *)proc_buffer)[sizeof(proc_buffer) - 1] = 0x78;
    assert(((volatile unsigned char *)startup_maps)[0] == 0x12);
    assert(((volatile unsigned char *)startup_maps)[sizeof(startup_maps) - 1] == 0x34);
    assert(((volatile unsigned char *)proc_buffer)[0] == 0x56);
    assert(((volatile unsigned char *)proc_buffer)[sizeof(proc_buffer) - 1] == 0x78);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "storage") == 0) {
        startup_storage();
        puts("fault startup NOBITS bounds, zeroing, alignment and mutable scratch passed");
        return 0;
    }
    assert(argc == 1);
    parser();
    no_new_mappings();
    puts("fault startup parser, ordering, address-space and immutable pools passed");
    return 0;
}
