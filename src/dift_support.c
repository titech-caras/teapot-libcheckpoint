#include "dift_support.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE MAP_FIXED
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

dift_tag_t dift_reg_tags[DIFT_REG_TAGS_SIZE] LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(16);

/*
 * If a manual tag update is required as a result of a gadget policy,
 * it is buffered here first and updated after DIFT propagation.
 */
dift_tag_t dift_reg_queued_tags[DIFT_REG_TAGS_SIZE] LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(16);

#define MAX_RUNTIME_MAPPED_RANGES 64
struct mapped_range {
    uintptr_t start;
    uintptr_t end;
};

static struct mapped_range runtime_mapped_ranges[MAX_RUNTIME_MAPPED_RANGES];
static size_t runtime_mapped_range_count = 0;

static uintptr_t page_size(void) {
    long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? (uintptr_t)size : 4096;
}

static uintptr_t page_down(uintptr_t addr) {
    uintptr_t page_mask = page_size() - 1;
    return addr & ~page_mask;
}

static uintptr_t page_up(uintptr_t addr) {
    uintptr_t page_mask = page_size() - 1;
    return (addr + page_mask) & ~page_mask;
}

static void map_fixed_pages(uintptr_t start, uintptr_t end, int prot) {
    size_t len = end - start;
    int flags = MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE | MAP_NORESERVE;
    errno = 0;
    void *addr = mmap((void *)start, len, prot, flags, -1, 0);
    if (addr == (void *)start) {
        return;
    }

    int saved_errno = errno;
    if (addr != MAP_FAILED) {
        munmap(addr, len);
        saved_errno = EEXIST;
    }

    fprintf(stderr, "Map address 0x%llx len 0x%llx failed: %s\n",
            (unsigned long long)start,
            (unsigned long long)len,
            strerror(saved_errno));
    abort();
}

static void remember_runtime_range(uintptr_t start, uintptr_t end) {
    if (start >= end) {
        return;
    }

    for (size_t i = 0; i < runtime_mapped_range_count; i++) {
        struct mapped_range *range = &runtime_mapped_ranges[i];
        if (end < range->start || start > range->end) {
            continue;
        }

        if (start < range->start)
            range->start = start;
        if (end > range->end)
            range->end = end;
        return;
    }

    if (runtime_mapped_range_count == MAX_RUNTIME_MAPPED_RANGES) {
        fprintf(stderr, "Too many runtime mapped ranges\n");
        abort();
    }

    runtime_mapped_ranges[runtime_mapped_range_count].start = start;
    runtime_mapped_ranges[runtime_mapped_range_count].end = end;
    runtime_mapped_range_count++;
}

void map_runtime_shadow_range(uintptr_t start, uintptr_t end, int prot) {
    start = page_down(start);
    end = page_up(end);

    uintptr_t cursor = start;
    while (cursor < end) {
        uintptr_t map_end = end;
        bool covered = false;

        for (size_t i = 0; i < runtime_mapped_range_count; i++) {
            uintptr_t mapped_start = runtime_mapped_ranges[i].start;
            uintptr_t mapped_end = runtime_mapped_ranges[i].end;

            if (mapped_end <= cursor || mapped_start >= end) {
                continue;
            }
            if (mapped_start <= cursor && cursor < mapped_end) {
                cursor = mapped_end;
                covered = true;
                break;
            }
            if (cursor < mapped_start && mapped_start < map_end) {
                map_end = mapped_start;
            }
        }

        if (covered) {
            continue;
        }

        map_fixed_pages(cursor, map_end, prot);
        remember_runtime_range(cursor, map_end);
        cursor = map_end;
    }
}

static uintptr_t dift_xor_granularity(void) {
    return (uintptr_t)DIFT_XOR_MASK & (0 - (uintptr_t)DIFT_XOR_MASK);
}

static uintptr_t next_dift_xor_boundary(uintptr_t addr) {
    uintptr_t granularity = dift_xor_granularity();
    uintptr_t boundary = (addr & ~(granularity - 1)) + granularity;
    return boundary > addr ? boundary : UINTPTR_MAX;
}

static void map_contiguous_dift_shadow_for_app_range(uintptr_t app_start, uintptr_t app_end) {
    if (app_end <= app_start) {
        return;
    }

    uintptr_t shadow_a = (uintptr_t)DIFT_MEM_ADDR(app_start);
    uintptr_t shadow_b = (uintptr_t)DIFT_MEM_ADDR(app_end - 1) + 1;
    uintptr_t shadow_start = shadow_a < shadow_b ? shadow_a : shadow_b;
    uintptr_t shadow_end = shadow_a < shadow_b ? shadow_b : shadow_a;
    map_runtime_shadow_range(shadow_start, shadow_end, PROT_READ | PROT_WRITE);
}

static void map_dift_shadow_for_app_range(uintptr_t app_start, uintptr_t app_end) {
    if (app_end <= app_start) {
        return;
    }

    uintptr_t cursor = app_start;
    while (cursor < app_end) {
        uintptr_t boundary = next_dift_xor_boundary(cursor);
        uintptr_t chunk_end = app_end < boundary ? app_end : boundary;
        map_contiguous_dift_shadow_for_app_range(cursor, chunk_end);
        cursor = chunk_end;
    }
}

void map_dift_pages() {
#ifdef DIFT_APP_RANGE0_START
    map_dift_shadow_for_app_range(DIFT_APP_RANGE0_START, DIFT_APP_RANGE0_END);
#endif
#ifdef DIFT_APP_RANGE1_START
    map_dift_shadow_for_app_range(DIFT_APP_RANGE1_START, DIFT_APP_RANGE1_END);
#endif
#ifdef DIFT_APP_RANGE2_START
    map_dift_shadow_for_app_range(DIFT_APP_RANGE2_START, DIFT_APP_RANGE2_END);
#endif
#ifdef DIFT_APP_RANGE3_START
    map_dift_shadow_for_app_range(DIFT_APP_RANGE3_START, DIFT_APP_RANGE3_END);
#endif
#ifdef DIFT_APP_RANGE4_START
    map_dift_shadow_for_app_range(DIFT_APP_RANGE4_START, DIFT_APP_RANGE4_END);
#endif
}

__attribute__((noinline)) void dift_set_mem_tags(void *addr, dift_tag_t tag, size_t len) {
    volatile dift_tag_t *tags = DIFT_MEM_ADDR(addr);
    for (size_t i = 0; i < len; i++) {
        tags[i] = tag;
    }
}

__attribute__((noinline)) void dift_copy_mem_tags(void *dest, const void *src, size_t len) {
    volatile dift_tag_t *dest_tags = DIFT_MEM_ADDR(dest);
    volatile dift_tag_t *src_tags = DIFT_MEM_ADDR(src);
    for (size_t i = 0; i < len; i++) {
        dest_tags[i] = src_tags[i];
    }
}

__attribute__((noinline)) void dift_move_mem_tags(void *dest, const void *src, size_t len) {
    volatile dift_tag_t *dest_tags = DIFT_MEM_ADDR(dest);
    volatile dift_tag_t *src_tags = DIFT_MEM_ADDR(src);
    if (dest_tags < src_tags) {
        for (size_t i = 0; i < len; i++) {
            dest_tags[i] = src_tags[i];
        }
    } else if (dest_tags > src_tags) {
        for (size_t i = len; i > 0; i--) {
            dest_tags[i - 1] = src_tags[i - 1];
        }
    }
}

void dift_taint_args(int argc, char **argv) {
    // Taint source: argc and argv.

    dift_reg_tags[DIFT_ARG0] = TAG_ATTACKER;

    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]);
        dift_set_mem_tags(argv[i], TAG_ATTACKER, len);
    }
}
