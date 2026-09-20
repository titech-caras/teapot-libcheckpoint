#define _GNU_SOURCE
#undef NDEBUG
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dift_support.h"

extern void *calloc__dift_wrapper__(size_t, size_t);
extern char *strdup__dift_wrapper__(const char *);
extern void *memcpy__dift_wrapper__(void *, const void *, size_t);
extern void *__memcpy_chk__dift_wrapper__(void *, const void *, size_t, size_t);
extern void *memmove__dift_wrapper__(void *, const void *, size_t);
extern void *memset__dift_wrapper__(void *, int, size_t);
extern void *__memset_chk__dift_wrapper__(void *, int, size_t, size_t);
extern char *strcpy__dift_wrapper__(char *, const char *);
extern char *strcat__dift_wrapper__(char *, const char *);
extern char *strncat__dift_wrapper__(char *, const char *, size_t);
extern char *__strncat_chk__dift_wrapper__(char *, const char *, size_t, size_t);
extern char *strncpy__dift_wrapper__(char *, const char *, size_t);
extern int inet_pton__dift_wrapper__(int, const char *, void *);
extern size_t fread__dift_wrapper__(void *, size_t, size_t, FILE *);
extern size_t fread_unlocked__dift_wrapper__(void *, size_t, size_t, FILE *);
extern char *fgets__dift_wrapper__(char *, int, FILE *);
extern char *__real_strdup(const char *);

dift_tag_t dift_reg_tags[DIFT_REG_TAGS_SIZE];
dift_tag_t dift_reg_queued_tags[DIFT_REG_TAGS_SIZE];
static unsigned char *arena;
static size_t arena_size;
static bool fail_strdup;
static struct {
    void *dest;
    const void *src;
    size_t length;
    dift_tag_t tag;
    unsigned count;
} effect;
static unsigned *shared_effect_count;

char *__wrap_strdup(const char *source) {
    if (fail_strdup) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_strdup(source);
}

static bool in_arena(const void *address, size_t size) {
    uintptr_t base = (uintptr_t)arena, pointer = (uintptr_t)address;
    return base <= pointer && pointer - base <= arena_size && size <= arena_size - (pointer - base);
}

/* Record allocation-result effects without reserving arbitrary heap shadows. */
void dift_set_mem_tags(void *address, dift_tag_t tag, size_t size) {
    effect.dest = address;
    effect.src = NULL;
    effect.length = size;
    effect.tag = tag;
    effect.count++;
    if (shared_effect_count) (*shared_effect_count)++;
    if (in_arena(address, size)) memset(DIFT_MEM_ADDR(address), tag, size);
}

void dift_copy_mem_tags(void *dest, const void *source, size_t size) {
    effect.dest = dest;
    effect.src = source;
    effect.length = size;
    effect.count++;
    if (shared_effect_count) (*shared_effect_count)++;
    if (in_arena(dest, size) && in_arena(source, size))
        memcpy(DIFT_MEM_ADDR(dest), DIFT_MEM_ADDR(source), size);
}

void dift_move_mem_tags(void *dest, const void *source, size_t size) {
    effect.dest = dest;
    effect.src = source;
    effect.length = size;
    effect.count++;
    if (shared_effect_count) (*shared_effect_count)++;
    if (in_arena(dest, size) && in_arena(source, size))
        memmove(DIFT_MEM_ADDR(dest), DIFT_MEM_ADDR(source), size);
}

static void reset(void) {
    memset(arena, 0xa5, arena_size);
    memset(DIFT_MEM_ADDR(arena), 0x40, arena_size);
    memset(dift_reg_tags, 0x20, sizeof(dift_reg_tags));
    dift_reg_tags[DIFT_ARG0] = 0x12;
    dift_reg_tags[DIFT_ARG1] = 0x11;
    memset(&effect, 0, sizeof(effect));
}

static void allocations(void) {
    void *result = calloc__dift_wrapper__(SIZE_MAX, 2);
    assert(result == NULL && effect.count == 0);
    result = calloc__dift_wrapper__(3, 7);
    assert(result != NULL && effect.count == 1);
    assert(effect.dest == result && effect.length == 21 && effect.tag == 0);
    for (size_t i = 0; i < 21; i++) assert(((unsigned char *)result)[i] == 0);
    free(result);

    char *source = (char *)arena + 32;
    strcpy(source, "value");
    effect.count = 0;
    fail_strdup = true;
    errno = 0;
    assert(strdup__dift_wrapper__(source) == NULL && errno == ENOMEM && effect.count == 0);
    fail_strdup = false;
    result = strdup__dift_wrapper__(source);
    assert(result != NULL && strcmp(result, source) == 0);
    assert(effect.count == 1 && effect.dest == result && effect.src == source && effect.length == 6);
    free(result);
}

static void memory(void) {
    unsigned char *src = arena + 32, *dest = arena + 128;
    for (unsigned i = 0; i < 24; i++) {
        src[i] = i;
        DIFT_MEM_TAG(src + i) = i + 1;
    }
    assert(memcpy__dift_wrapper__(dest, src, 12) == dest);
    assert(memcmp(dest, src, 12) == 0 && memcmp(DIFT_MEM_ADDR(dest), DIFT_MEM_ADDR(src), 12) == 0);
    assert(dift_reg_tags[DIFT_RET] == 0x12);
    assert(__memcpy_chk__dift_wrapper__(dest, src, 16, 24) == dest);
    assert(dest[16] == 0xa5 && DIFT_MEM_TAG(dest + 16) == 0x40);
    assert(memmove__dift_wrapper__(src + 2, src, 16) == src + 2);
    for (unsigned i = 0; i < 16; i++) assert(src[i + 2] == i && DIFT_MEM_TAG(src + i + 2) == i + 1);
    assert(memset__dift_wrapper__(dest, 0x5a, 12) == dest);
    assert(__memset_chk__dift_wrapper__(dest, 0x5b, 12, 24) == dest);
    for (unsigned i = 0; i < 12; i++) assert(dest[i] == 0x5b && DIFT_MEM_TAG(dest + i) == 0x11);
    assert(dest[12] == 12 && DIFT_MEM_TAG(dest + 12) == 13);
}

static void strings(void) {
    char *src = (char *)arena + 32, *dest = (char *)arena + 128;
    strcpy(src, "abc");
    for (unsigned i = 0; i < 4; i++) DIFT_MEM_TAG(src + i) = i + 1;
    assert(strcpy__dift_wrapper__(dest, src) == dest && strcmp(dest, src) == 0);
    assert(memcmp(DIFT_MEM_ADDR(dest), DIFT_MEM_ADDR(src), 4) == 0);
    assert(dest[4] == (char)0xa5 && DIFT_MEM_TAG(dest + 4) == 0x40);

    strcpy(dest, "prefix-");
    memset(DIFT_MEM_ADDR(dest), 0x40, 24);
    assert(strcat__dift_wrapper__(dest, src) == dest && strcmp(dest, "prefix-abc") == 0);
    assert(memcmp(DIFT_MEM_ADDR(dest + 7), DIFT_MEM_ADDR(src), 4) == 0);
    assert(DIFT_MEM_TAG(dest + 6) == 0x40 && DIFT_MEM_TAG(dest + 11) == 0x40);

    for (size_t limit = 0; limit <= 5; limit++) {
        for (unsigned checked = 0; checked < 2; checked++) {
            strcpy(dest, "prefix-");
            memset(DIFT_MEM_ADDR(dest), 0x40, 24);
            size_t copied = limit < 3 ? limit : 3;
            char *result = checked ? __strncat_chk__dift_wrapper__(dest, src, limit, 24)
                                   : strncat__dift_wrapper__(dest, src, limit);
            assert(result == dest && strlen(dest) == 7 + copied);
            assert(memcmp(dest + 7, src, copied) == 0);
            for (size_t i = 0; i < copied; i++) assert(DIFT_MEM_TAG(dest + 7 + i) == i + 1);
            assert(DIFT_MEM_TAG(dest + 7 + copied) == (copied < limit ? 4 : 0));
            assert(DIFT_MEM_TAG(dest + 6) == 0x40 && DIFT_MEM_TAG(dest + 8 + copied) == 0x40);
        }
    }
    memset(DIFT_MEM_ADDR(dest), 0x40, 24);
    assert(strncpy__dift_wrapper__(dest, src, 8) == dest);
    assert(memcmp(DIFT_MEM_ADDR(dest), DIFT_MEM_ADDR(src), 3) == 0);
    for (unsigned i = 3; i < 8; i++) assert(dest[i] == 0 && DIFT_MEM_TAG(dest + i) == 0);
    assert(DIFT_MEM_TAG(dest + 8) == 0x40);
}

static void addresses(void) {
    const char *sources[] = {"192.0.2.1", "2001:db8::1", "invalid", "invalid"};
    const int families[] = {AF_INET, AF_INET6, AF_INET, -1};
    char *source = (char *)arena + 32;
    void *dest = arena + 128;
    for (unsigned i = 0; i < 4; i++) {
        reset();
        strcpy(source, sources[i]);
        DIFT_MEM_TAG(source) = TAG_ATTACKER;
        unsigned char expected[32];
        memset(expected, 0xa5, sizeof(expected));
        errno = 0;
        int status = inet_pton(families[i], source, expected), saved_errno = errno;
        errno = 0;
        assert(inet_pton__dift_wrapper__(families[i], source, dest) == status && errno == saved_errno);
        assert(memcmp(dest, expected, sizeof(expected)) == 0);
        size_t written = status == 1 ? (families[i] == AF_INET ? 4 : 16) : 0;
        for (size_t j = 0; j < sizeof(expected); j++)
            assert(DIFT_MEM_TAG((unsigned char *)dest + j) == (j < written ? TAG_ATTACKER : 0x40));
        assert(effect.count == (status == 1));
    }
}

struct input_source {
    const unsigned char *data;
    size_t length;
    size_t offset;
    int terminal_errno;
    bool priming;
};

static ssize_t input_read(void *cookie, char *buffer, size_t size) {
    struct input_source *input = cookie;
    if (input->priming || input->offset == input->length) {
        if (!input->terminal_errno) return 0;
        errno = input->terminal_errno;
        return -1;
    }
    size_t remaining = input->length - input->offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, input->data + input->offset, size);
    input->offset += size;
    return size;
}

static FILE *open_input(unsigned mode, struct input_source *input) {
    FILE *stream;
    if (mode == 0) {
        stream = tmpfile();
        assert(stream != NULL);
        assert(fwrite(input->data, 1, input->length, stream) == input->length);
        rewind(stream);
    } else if (mode == 1) {
        int descriptors[2];
        assert(pipe(descriptors) == 0);
        assert(write(descriptors[1], input->data, input->length) == (ssize_t)input->length);
        assert(close(descriptors[1]) == 0);
        stream = fdopen(descriptors[0], "r");
    } else {
        cookie_io_functions_t functions = {.read = input_read};
        stream = fopencookie(input, "r", functions);
    }
    assert(stream != NULL);
    return stream;
}

static void block_input(void) {
    const unsigned char data[] = "abcdefghijklmnop";
    const size_t lengths[] = {0, 3, 8, 9, 13, 16};
    const size_t sizes[] = {0, 1, 4, 8};
    for (unsigned unlocked = 0; unlocked < 2; ++unlocked) {
        for (unsigned mode = 0; mode < 3; ++mode) {
            for (unsigned length = 0; length < sizeof(lengths) / sizeof(lengths[0]); ++length) {
                for (unsigned size = 0; size < sizeof(sizes) / sizeof(sizes[0]); ++size) {
                    for (size_t count = 0; count <= 4; ++count) {
                        reset();
                        struct input_source source = {data, lengths[length], 0, EIO, false};
                        FILE *stream = open_input(mode, &source);
                        unsigned char expected[64];
                        memset(expected, 0xa5, sizeof(expected));
                        errno = 0;
                        size_t result = unlocked ? fread_unlocked(expected, sizes[size], count, stream)
                                                 : fread(expected, sizes[size], count, stream);
                        int saved_errno = errno, eof = feof(stream), error = ferror(stream);
                        assert(fclose(stream) == 0);
                        source.offset = 0;
                        stream = open_input(mode, &source);
                        errno = 0;
                        size_t actual = unlocked ? fread_unlocked__dift_wrapper__(arena, sizes[size], count, stream)
                                                 : fread__dift_wrapper__(arena, sizes[size], count, stream);
                        assert(actual == result && errno == saved_errno);
                        assert(feof(stream) == eof && ferror(stream) == error);
                        assert(fclose(stream) == 0);
                        assert(memcmp(arena, expected, sizeof(expected)) == 0);
                        size_t written = sizes[size] * count;
                        if (written > source.length) written = source.length;
                        for (size_t i = 0; i < sizeof(expected); ++i)
                            assert(DIFT_MEM_TAG(arena + i) == (i < written ? TAG_ATTACKER : 0x40));
                    }
                }
            }
        }
    }
}

static void line_input(void) {
    // Fortification may select __fgets_chk, a different wrapper target with
    // different n == 1 behavior in older glibc. Compare the actual fgets ABI.
    char *(*volatile read_line)(char *, int, FILE *) = fgets;
    const unsigned char data[] = {'A', 0, 'B', '\r', 'C', '\n', 'D'};
    const int counts[] = {-3, 0, 1, 2, 4, 6, 8, 16};
    const int errors[] = {0, EIO, EAGAIN, EINTR};
    for (unsigned mode = 0; mode < 3; mode++) {
        for (unsigned err = 0; err < sizeof(errors) / sizeof(errors[0]); err++) {
            for (unsigned primed = 0; primed < 2; primed++) {
                if (mode != 2 && (err || primed)) continue;
                for (size_t length = 0; length <= sizeof(data); length++) {
                    for (unsigned size = 0; size < sizeof(counts) / sizeof(counts[0]); size++) {
                        reset();
                        unsigned char expected[32];
                        // Alternate old contents so byte-difference/strlen guesses fail.
                        memset(expected, length % 2 ? 0 : 0xa5, sizeof(expected));
                        memcpy(arena, expected, sizeof(expected));
                        int n = counts[size];
                        int result = 0, saved_errno = 0, eof = 0, error = 0;
                        for (unsigned wrapped = 0; wrapped < 2; wrapped++) {
                            struct input_source source = {data, length, 0, errors[err], primed != 0};
                            FILE *stream = open_input(mode, &source);
                            if (primed) {
                                assert(fgetc(stream) == EOF);
                                source.priming = false;
                            }
                            errno = 0;
                            if (!wrapped) {
                                result = read_line((char *)expected, n, stream) != NULL;
                                saved_errno = errno;
                                eof = feof(stream);
                                error = ferror(stream);
                            } else {
                                char *actual = fgets__dift_wrapper__((char *)arena, n, stream);
                                if (actual != (result ? (char *)arena : NULL))
                                    fprintf(stderr, "line input: mode=%u err=%d primed=%u length=%zu n=%d result=%d actual=%p errno=%d/%d flags=%d,%d/%d,%d\n",
                                            mode, errors[err], primed, length, n, result, (void *)actual,
                                            errno, saved_errno, feof(stream), ferror(stream), eof, error);
                                assert(actual == (result ? (char *)arena : NULL));
                                assert(errno == saved_errno && feof(stream) == eof && ferror(stream) == error);
                                assert(dift_reg_tags[DIFT_RET] == (result ? 0x12 : 0));
                            }
                            assert(fclose(stream) == 0);
                        }
                        assert(memcmp(arena, expected, sizeof(expected)) == 0);
                        size_t copied = 0;
                        if (n > 1 && !(primed && !errors[err])) {
                            while (copied < length && copied < (size_t)n - 1) {
                                if (data[copied++] == '\n') break;
                            }
                        }
                        for (size_t i = 0; i < sizeof(expected); i++) {
                            dift_tag_t tag = i < copied ? TAG_ATTACKER : 0x40;
                            if (result && i == copied) tag = 0;
                            assert(DIFT_MEM_TAG(arena + i) == tag);
                        }
                    }
                }
            }
        }
    }
}

static void fortified_failure(void) {
    shared_effect_count = mmap(NULL, arena_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(shared_effect_count != MAP_FAILED);
    for (unsigned op = 0; op < 3; op++) {
        *shared_effect_count = 0;
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            char *source = (char *)arena + 32, *dest = (char *)arena + 128;
            strcpy(source, "too long");
            strcpy(dest, "prefix");
            if (op == 0) __memcpy_chk__dift_wrapper__(dest, source, 8, 4);
            if (op == 1) __memset_chk__dift_wrapper__(dest, 0, 8, 4);
            if (op == 2) __strncat_chk__dift_wrapper__(dest, source, 8, 4);
            _exit(99);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
        assert(*shared_effect_count == 0);
    }
    assert(munmap(shared_effect_count, arena_size) == 0);
    shared_effect_count = NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    struct rlimit core_limit = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &core_limit) == 0);
    arena_size = (size_t)sysconf(_SC_PAGESIZE);
    arena = mmap((void *)0x30000000, arena_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(arena == (void *)0x30000000);
    void *shadow = mmap(DIFT_MEM_ADDR(arena), arena_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(shadow == DIFT_MEM_ADDR(arena));
    reset();
    if (strcmp(argv[1], "allocations") == 0) allocations();
    else if (strcmp(argv[1], "memory") == 0) memory();
    else if (strcmp(argv[1], "strings") == 0) strings();
    else if (strcmp(argv[1], "addresses") == 0) addresses();
    else if (strcmp(argv[1], "block-input") == 0) block_input();
    else if (strcmp(argv[1], "line-input") == 0) line_input();
    else if (strcmp(argv[1], "fortified-failure") == 0) fortified_failure();
    else return 2;
    assert(munmap(shadow, arena_size) == 0);
    assert(munmap(arena, arena_size) == 0);
    return 0;
}
