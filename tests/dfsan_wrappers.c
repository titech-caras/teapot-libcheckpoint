// LLVM DFSan's wrappers through Teapot's bridges: libc's results, and the
// labels of DFSan's rules with strict_data_dependencies off.
#define _DEFAULT_SOURCE
#undef NDEBUG
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dift_support.h"

extern void *memcpy__dift_wrapper__(void *, const void *, size_t);
extern void *memmove__dift_wrapper__(void *, const void *, size_t);
extern void *memset__dift_wrapper__(void *, int, size_t);
extern char *strcpy__dift_wrapper__(char *, const char *);
extern char *strcat__dift_wrapper__(char *, const char *);
extern char *strdup__dift_wrapper__(const char *);
extern size_t strlen__dift_wrapper__(const char *);
extern char *strchr__dift_wrapper__(const char *, int);
extern char *strrchr__dift_wrapper__(const char *, int);
extern char *strpbrk__dift_wrapper__(const char *, const char *);
extern char *strstr__dift_wrapper__(const char *, const char *);
extern void *memchr__dift_wrapper__(const void *, int, size_t);
extern int memcmp__dift_wrapper__(const void *, const void *, size_t);
extern int bcmp__dift_wrapper__(const void *, const void *, size_t);
extern int strcmp__dift_wrapper__(const char *, const char *);
extern int strncmp__dift_wrapper__(const char *, const char *, size_t);
extern int strcasecmp__dift_wrapper__(const char *, const char *);
extern int strncasecmp__dift_wrapper__(const char *, const char *, size_t);
extern long strtol__dift_wrapper__(const char *, char **, int);
extern long long strtoll__dift_wrapper__(const char *, char **, int);
extern unsigned long strtoul__dift_wrapper__(const char *, char **, int);
extern unsigned long long strtoull__dift_wrapper__(const char *, char **, int);
extern long __isoc23_strtol__dift_wrapper__(const char *, char **, int);
extern long __isoc23_strtol(const char *, char **, int) __attribute__((weak));
extern int atoi__dift_wrapper__(const char *);
extern int inet_pton__dift_wrapper__(int, const char *, void *);

dift_tag_t dift_reg_tags[DIFT_REG_TAGS_SIZE];
static unsigned char *arena;
static size_t arena_size;

static int in_arena(const void *address, size_t size) {
    uintptr_t base = (uintptr_t)arena, pointer = (uintptr_t)address;
    return base <= pointer && pointer - base <= arena_size && size <= arena_size - (pointer - base);
}

// Only the arena has a shadow; strdup's heap copy is not tracked here.
void dift_set_mem_tags(void *address, dift_tag_t tag, size_t size) {
    if (in_arena(address, size)) memset(DIFT_MEM_ADDR(address), tag, size);
}

void dift_move_mem_tags(void *dest, const void *source, size_t size) {
    if (in_arena(dest, size) && in_arena(source, size))
        memmove(DIFT_MEM_ADDR(dest), DIFT_MEM_ADDR(source), size);
}

// A copy of bytes in the arena, every byte's tag set to tag.
static char *put(size_t offset, const void *bytes, size_t size, dift_tag_t tag) {
    char *copy = (char *)arena + offset;
    memcpy(copy, bytes, size);
    memset(DIFT_MEM_ADDR(copy), tag, size);
    return copy;
}

static void set_tags(const void *address, const dift_tag_t *tags, size_t size) {
    memcpy(DIFT_MEM_ADDR(address), tags, size);
}

static void results(void) {
    static const char *const pairs[][2] = {
        {"\x80", "\x01"}, {"\x01", "\x80"}, {"abc", "abd"}, {"abc", "abc"},
        {"ab", "abc"}, {"\xe9t\xe9", "\xc9T\xc9"}, {"A", "a"}, {"", "\xff"},
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        char *a = put(0, pairs[i][0], strlen(pairs[i][0]) + 1, 0);
        char *b = put(64, pairs[i][1], strlen(pairs[i][1]) + 1, 0);
        size_t n = strlen(a) < strlen(b) ? strlen(a) + 1 : strlen(b) + 1;
        // The comparison results are libc's own.
        assert(memcmp__dift_wrapper__(a, b, n) == memcmp(a, b, n));
        assert(bcmp__dift_wrapper__(a, b, n) == bcmp(a, b, n));
        assert(strcmp__dift_wrapper__(a, b) == strcmp(a, b));
        assert(strcasecmp__dift_wrapper__(a, b) == strcasecmp(a, b));
        for (size_t limit = 0; limit <= 4; limit++) {
            assert(strncmp__dift_wrapper__(a, b, limit) == strncmp(a, b, limit));
            assert(strncasecmp__dift_wrapper__(a, b, limit) == strncasecmp(a, b, limit));
        }
    }

    char *s = put(0, "a\xe9" "bcb", 6, 0);
    static const int chars[] = {0xe9, -23, 'b', 0, 256 + 'b', 'z', 'a'};
    for (size_t i = 0; i < sizeof(chars) / sizeof(chars[0]); i++) {
        assert(strchr__dift_wrapper__(s, chars[i]) == strchr(s, chars[i]));
        assert(strrchr__dift_wrapper__(s, chars[i]) == strrchr(s, chars[i]));
        assert(memchr__dift_wrapper__(s, chars[i], 6) == memchr(s, chars[i], 6));
    }
    char *set = put(64, "cz", 3, 0), *needle = put(96, "bc", 3, 0);
    assert(strpbrk__dift_wrapper__(s, set) == strpbrk(s, set));
    assert(strstr__dift_wrapper__(s, needle) == strstr(s, needle));
    assert(strlen__dift_wrapper__(s) == strlen(s));

    static const char *const numbers[] = {" -42x", "0x1F", "0b101", "99999999999999999999", "", "077"};
    static const int bases[] = {0, 2, 10, 16};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        char *text = put(0, numbers[i], strlen(numbers[i]) + 1, 0);
        for (size_t j = 0; j < sizeof(bases) / sizeof(bases[0]); j++) {
            char *expected_end, *end;
            errno = 0;
            long long expected = strtoll(text, &expected_end, bases[j]);
            int expected_errno = errno;
            errno = 0;
            assert(strtoll__dift_wrapper__(text, &end, bases[j]) == expected);
            assert(end == expected_end && errno == expected_errno);
            errno = 0;
            unsigned long expected_unsigned = strtoul(text, &expected_end, bases[j]);
            expected_errno = errno;
            errno = 0;
            assert(strtoul__dift_wrapper__(text, &end, bases[j]) == expected_unsigned);
            assert(end == expected_end && errno == expected_errno);
            assert(strtol__dift_wrapper__(text, NULL, bases[j]) == strtol(text, NULL, bases[j]));
            assert(strtoull__dift_wrapper__(text, NULL, bases[j]) == strtoull(text, NULL, bases[j]));
            if (__isoc23_strtol != NULL) {
                assert(__isoc23_strtol__dift_wrapper__(text, &end, bases[j]) ==
                       __isoc23_strtol(text, &expected_end, bases[j]));
                assert(end == expected_end);
            }
        }
        assert(atoi__dift_wrapper__(text) == atoi(text));
    }
    // The ISO bridge stops at the C23 prefix; the C23 bridge accepts it.
    char *binary = put(0, "0b101", 6, 0);
    assert(strtol__dift_wrapper__(binary, NULL, 0) == 0);
    if (__isoc23_strtol != NULL) assert(__isoc23_strtol__dift_wrapper__(binary, NULL, 0) == 5);
}

static void labels(void) {
    static const dift_tag_t abc[] = {1, 2, 4, 8};
    char *s = put(0, "abc", 4, 0);
    set_tags(s, abc, 4);
    assert(strlen__dift_wrapper__(s) == 3 && dift_reg_tags[DIFT_RET] == 15);

    dift_reg_tags[DIFT_ARG0] = 0x10;
    dift_reg_tags[DIFT_ARG1] = 0x20;
    assert(strchr__dift_wrapper__(s, 'b') == s + 1 && dift_reg_tags[DIFT_RET] == (1 | 2 | 0x10 | 0x20));

    // Comparisons take the tags of the bytes read through the first difference.
    char *x = put(64, "abX", 4, 0x10), *y = put(96, "abY", 4, 0x20);
    static const dift_tag_t first[] = {1, 1, 2, 0x40}, second[] = {4, 4, 8, 0x80};
    set_tags(x, first, 4);
    set_tags(y, second, 4);
    assert(strcmp__dift_wrapper__(x, y) < 0 && dift_reg_tags[DIFT_RET] == 15);
    assert(memcmp__dift_wrapper__(x, y, 2) == 0 && dift_reg_tags[DIFT_RET] == 5);

    char *dest = put(128, "........", 8, 0x40);
    dift_reg_tags[DIFT_ARG0] = 0x12;
    assert(memcpy__dift_wrapper__(dest, s, 4) == dest && dift_reg_tags[DIFT_RET] == 0x12);
    assert(memcmp(DIFT_MEM_ADDR(dest), abc, 4) == 0 && DIFT_MEM_TAG(dest + 4) == 0x40);
    dift_reg_tags[DIFT_ARG1] = 0x11;
    assert(memset__dift_wrapper__(dest, 'z', 3) == dest);
    for (int i = 0; i < 3; i++) assert(DIFT_MEM_TAG(dest + i) == 0x11);

    char *number = put(0, "12x", 4, 0);
    static const dift_tag_t digits[] = {1, 2, 4, 8};
    set_tags(number, digits, 4);
    dift_reg_tags[DIFT_ARG2] = 0x40;
    assert(strtol__dift_wrapper__(number, NULL, 10) == 12 && dift_reg_tags[DIFT_RET] == (1 | 2 | 0x40));
    number[2] = 0;
    assert(atoi__dift_wrapper__(number) == 12 && dift_reg_tags[DIFT_RET] == (1 | 2 | 4));

    char *address = put(0, "192.0.2.1", 10, 1);
    DIFT_MEM_TAG(address + 4) = 2;
    unsigned char *bytes = (unsigned char *)put(64, "\0\0\0\0\0", 5, 0x40);
    assert(inet_pton__dift_wrapper__(AF_INET, address, bytes) == 1 && dift_reg_tags[DIFT_RET] == 0);
    for (int i = 0; i < 4; i++) assert(DIFT_MEM_TAG(bytes + i) == 3);
    assert(DIFT_MEM_TAG(bytes + 4) == 0x40);

    char *copy = strdup__dift_wrapper__(s);
    assert(copy != NULL && strcmp(copy, s) == 0 && dift_reg_tags[DIFT_RET] == 0);
    free(copy);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    arena_size = (size_t)sysconf(_SC_PAGESIZE);
    arena = mmap((void *)0x30000000, arena_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(arena == (void *)0x30000000);
    void *shadow = mmap(DIFT_MEM_ADDR(arena), arena_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(shadow == DIFT_MEM_ADDR(arena));
    if (strcmp(argv[1], "results") == 0) results();
    else if (strcmp(argv[1], "labels") == 0) labels();
    else return 2;
    return 0;
}
