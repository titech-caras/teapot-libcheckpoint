#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>

static unsigned protect_calls;

/* The preinit reservations are outside this map-selection test. */
void map_dift_pages(void) {}

void map_runtime_shadow_range(uintptr_t start, uintptr_t end, int protection) {
    assert(start <= end);
    assert(protection == (PROT_READ | PROT_WRITE));
}

static FILE *test_fopen(const char *path, const char *mode) {
    static char maps[] =
        "1000-3000 rw-p 00000000 00:00 0 [stack]\n"
        "3000-4000 rw-p 00001000 08:01 42 /program\n"
        "4000-5000 r--p 00000000 00:00 0\n"
        "5000-7000 rw-p 00000000 00:00 0 [heap]\n"
        "7000-8000 rw-p 00000000 00:01 63 /memfd:untagged\n"
        "malformed line\n"
        "2000-3000 rw-p\n";
    assert(strcmp(path, "/proc/self/maps") == 0);
    assert(strcmp(mode, "r") == 0);
    return fmemopen(maps, sizeof(maps) - 1, "r");
}

static int test_mprotect(void *address, size_t size, int protection) {
    static void *expected[] = {(void *)0x2000, (void *)0x5000};
    assert(protect_calls < 2);
    assert(address == expected[protect_calls]);
    assert(size == 0x1000);
    assert(protection == (PROT_READ | PROT_WRITE | 0x20));
    protect_calls++;
    return 0;
}

/* Exercise the internal map walker without changing real process mappings. */
#define fopen test_fopen
#define mprotect test_mprotect
#include "../src/checkpoint.c"
#undef fopen
#undef mprotect

int main(int argc, char **argv) {
    assert(argc == 2);
    struct rlimit core_limit = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &core_limit) == 0);
    if (strcmp(argv[1], "mappings") == 0) {
        enable_mte_for_mapped_app_overlap(0x2000, 0x6000);
        assert(protect_calls == 2);
    } else {
        assert(strcmp(argv[1], "protected") == 0);
        /* This must also be a no-op on a CPU with no MTE instructions. */
        poison_protected_zone();
        assert(protect_calls == 0);
    }
    return 0;
}
