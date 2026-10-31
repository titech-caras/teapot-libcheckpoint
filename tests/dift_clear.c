// dift_set_mem_tags: exact bytes for small and nonzero ranges; a large zero
// range releases its whole shadow pages and still clears the partial ends.
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dift_support.h"

enum { ARENA = 1 << 20 };

static unsigned char *shadow;

static void fill(unsigned char value) { memset(shadow, value, ARENA); }

static void expect(size_t start, size_t end, unsigned char inside, unsigned char outside) {
    for (size_t i = 0; i < ARENA; i++)
        assert(shadow[i] == (i >= start && i < end ? inside : outside));
}

static size_t resident_pages(size_t start, size_t end) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE), count = 0;
    unsigned char state[ARENA / 4096 + 1];
    assert(mincore(shadow, ARENA, state) == 0);
    for (size_t i = (start + page - 1) / page; i < end / page; i++) count += state[i] & 1;
    return count;
}

int main(void) {
    unsigned char *arena = mmap((void *)0x30000000, ARENA, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(arena == (void *)0x30000000);
    shadow = mmap(DIFT_MEM_ADDR(arena), ARENA, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(shadow == (unsigned char *)DIFT_MEM_ADDR(arena));

    fill(0x5a);
    dift_set_mem_tags(arena + 100, 0, 1000);
    expect(100, 1100, 0, 0x5a);

    fill(0x5a);
    dift_set_mem_tags(arena + 100, 3, 300000);
    expect(100, 300100, 3, 0x5a);

    fill(0x5a);
    dift_set_mem_tags(arena + 100, 0, 300000);
    assert(resident_pages(100, 300100) == 0);  // before reading maps zero pages in
    expect(100, 300100, 0, 0x5a);
    return 0;
}
