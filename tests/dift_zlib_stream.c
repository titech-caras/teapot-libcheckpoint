#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#include "dift_support.h"

extern int inflate__dift_wrapper__(z_streamp, int);
extern int inflateInit___dift_wrapper__(z_streamp, const char *, int);
extern int inflateInit2___dift_wrapper__(z_streamp, int, const char *, int);
extern int inflateEnd__dift_wrapper__(z_streamp);
extern int inflateReset__dift_wrapper__(z_streamp);
extern int inflateReset2__dift_wrapper__(z_streamp, int);
extern int inflateResetKeep__dift_wrapper__(z_streamp);
extern int inflateCopy__dift_wrapper__(z_streamp, z_streamp);
extern int inflateSetDictionary__dift_wrapper__(z_streamp, const Bytef *, uInt);
extern int inflatePrime__dift_wrapper__(z_streamp, int, int);
extern int inflateSync__dift_wrapper__(z_streamp);

dift_tag_t dift_reg_tags[DIFT_REG_TAGS_SIZE];
static unsigned char *arena;
static const size_t arena_size = 0x10000;
static unsigned buffered_calls;
static unsigned allocations;
static int fail_allocation;

extern void *__real_malloc(size_t);
extern void __real_free(void *);

void *__wrap_malloc(size_t size) {
    if (fail_allocation) {
        errno = ENOMEM;
        return NULL;
    }
    void *result = __real_malloc(size);
    allocations += result != NULL;
    return result;
}

void __wrap_free(void *address) {
    if (address) {
        assert(allocations > 0);
        allocations--;
    }
    __real_free(address);
}

void dift_set_mem_tags(void *address, dift_tag_t tag, size_t size) {
    assert((uintptr_t)address >= (uintptr_t)arena);
    assert((uintptr_t)address + size <= (uintptr_t)arena + arena_size);
    memset(DIFT_MEM_ADDR(address), tag, size);
}

void dift_copy_mem_tags(void *dest, const void *source, size_t size) {
    memcpy(DIFT_MEM_ADDR(dest), DIFT_MEM_ADDR(source), size);
}

static void output_range(void) {
    z_streamp stream = (z_streamp)arena;
    unsigned char *input = arena + 4096, *output = arena + 8192;
    const unsigned char plain[] = "streamed output";
    uLongf size = 128;
    assert(compress2(input, &size, plain, sizeof(plain), Z_BEST_COMPRESSION) == Z_OK);
    assert(inflateInit___dift_wrapper__(stream, ZLIB_VERSION, sizeof(*stream)) == Z_OK);
    stream->next_in = input;
    stream->avail_in = size;
    stream->next_out = output;
    stream->avail_out = 128;
    memset(DIFT_MEM_ADDR(input), TAG_ATTACKER, size);
    memset(DIFT_MEM_ADDR(output), 0x40, 128);
    assert(inflate__dift_wrapper__(stream, Z_FINISH) == Z_STREAM_END);
    assert(memcmp(output, plain, sizeof(plain)) == 0);
    assert(stream->total_out == sizeof(plain));
    for (size_t i = 0; i < sizeof(plain); i++)
        assert(DIFT_MEM_TAG(output + i) == TAG_ATTACKER);
    for (size_t i = sizeof(plain); i < 128; i++)
        assert(DIFT_MEM_TAG(output + i) == 0x40);
    assert(inflateEnd__dift_wrapper__(stream) == Z_OK);
}

static uInt compressed_input(int window_bits, const unsigned char *dictionary, uInt dict_size) {
    unsigned char plain[4096];
    for (unsigned i = 0; i < sizeof(plain); i++)
        plain[i] = (unsigned char)('a' + i % 7);
    z_stream encoder = {0};
    assert(deflateInit2(&encoder, Z_BEST_COMPRESSION, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    if (dictionary)
        assert(deflateSetDictionary(&encoder, dictionary, dict_size) == Z_OK);
    encoder.next_in = plain;
    encoder.avail_in = sizeof(plain);
    encoder.next_out = arena + 4096;
    encoder.avail_out = 4096;
    assert(deflate(&encoder, Z_FINISH) == Z_STREAM_END);
    uInt size = encoder.total_out;
    assert(deflateEnd(&encoder) == Z_OK);
    return size;
}

static void initialize(z_streamp stream, z_streamp reference, int window_bits) {
    memset(stream, 0, sizeof(*stream));
    memset(reference, 0, sizeof(*reference));
    assert(inflateInit2___dift_wrapper__(stream, window_bits, ZLIB_VERSION, sizeof(*stream)) == Z_OK);
    assert(inflateInit2(reference, window_bits) == Z_OK);
}

/* Compare the actual zlib status, counters and every output byte, then check
 * tags against the consumed interval rather than the supplied capacity. */
static int step(z_streamp stream, z_streamp reference, int flush, uInt capacity, dift_tag_t *history) {
    unsigned char *input = stream->next_in, *output = arena + 8193;
    unsigned char expected[4098];
    assert(capacity <= sizeof(expected) - 2);
    uInt supplied = stream->avail_in;
    reference->next_in = stream->next_in;
    reference->avail_in = supplied;
    reference->next_out = expected + 1;
    reference->avail_out = capacity;
    stream->next_out = output;
    stream->avail_out = capacity;
    memset(output - 1, 0xa5, capacity + 2);
    memset(expected, 0xa5, capacity + 2);
    memset(DIFT_MEM_ADDR(output - 1), 0x40, capacity + 2);
    dift_set_mem_tags(&stream->next_out, 4, sizeof(stream->next_out));
    dift_set_mem_tags(&stream->avail_out, 8, sizeof(stream->avail_out));
    int result = inflate__dift_wrapper__(stream, flush);
    assert(result == inflate(reference, flush));
    assert(stream->avail_in == reference->avail_in);
    assert(stream->avail_out == reference->avail_out);
    assert(stream->next_in == reference->next_in);
    assert(stream->next_out == output + capacity - stream->avail_out);
    assert(stream->total_in == reference->total_in && stream->total_out == reference->total_out);
    assert(stream->adler == reference->adler && stream->data_type == reference->data_type);
    assert(memcmp(output - 1, expected, capacity + 2) == 0);
    for (uInt i = 0; i < supplied - stream->avail_in; i++)
        *history |= DIFT_MEM_TAG(input + i);
    uInt produced = capacity - stream->avail_out;
    if (produced && supplied == stream->avail_in)
        buffered_calls++;
    assert(DIFT_MEM_TAG(output - 1) == 0x40);
    for (uInt i = 0; i < capacity + 1; i++)
        assert(DIFT_MEM_TAG(output + i) == (i < produced ? *history : 0x40));
    assert(DIFT_MEM_TAG(&stream->next_out) == (produced ? (4 | *history) : 4));
    assert(DIFT_MEM_TAG(&stream->avail_out) == (produced ? (8 | *history) : 8));
    assert(dift_reg_tags[DIFT_RET] == *history);
    return result;
}

static void finish(z_streamp stream, z_streamp reference, dift_tag_t *history) {
    for (unsigned calls = 0; calls < 5000; calls++) {
        int result = step(stream, reference, Z_NO_FLUSH, 13, history);
        if (result == Z_STREAM_END)
            return;
        assert(result == Z_OK);
    }
    assert(0 && "stream failed to finish");
}

static void streaming(void) {
    z_streamp stream = (z_streamp)arena;
    z_stream reference;
    initialize(stream, &reference, 15);
    uInt size = compressed_input(15, NULL, 0);
    unsigned char *input = arena + 4096;
    memset(DIFT_MEM_ADDR(input), TAG_ATTACKER, size);
    DIFT_MEM_TAG(input) = 0;
    DIFT_MEM_TAG(input + size - 1) = TAG_SECRET;
    memset(DIFT_MEM_ADDR(input + size), TAG_SECRET_INDIRECT, 16);
    stream->next_in = input;
    stream->avail_in = 1;
    dift_tag_t history = 0;
    assert(step(stream, &reference, Z_NO_FLUSH, 1, &history) == Z_OK);
    assert(stream->total_out == 0 && history == 0);
    stream->avail_in = size - 1 + 16;
    finish(stream, &reference, &history);
    assert(history == (TAG_ATTACKER | TAG_SECRET));
    assert(stream->avail_in == 16 && buffered_calls > 0);
    assert(inflateEnd__dift_wrapper__(stream) == inflateEnd(&reference));
}

static void lifetime(void) {
    z_streamp stream = (z_streamp)arena, copy = stream + 1;
    z_stream reference, reference_copy;
    initialize(stream, &reference, -15);
    uInt size = compressed_input(-15, NULL, 0);
    memset(DIFT_MEM_ADDR(arena + 4096), TAG_ATTACKER, size);
    stream->next_in = arena + 4096;
    stream->avail_in = size;
    dift_tag_t history = 0;
    assert(step(stream, &reference, Z_NO_FLUSH, 1, &history) == Z_OK);
    assert(history == TAG_ATTACKER);
    assert(inflateCopy__dift_wrapper__(copy, stream) == Z_OK);
    assert(inflateCopy(&reference_copy, &reference) == Z_OK);
    dift_tag_t copy_history = history;
    memset(DIFT_MEM_ADDR(arena + 4096), TAG_SECRET, size);
    finish(copy, &reference_copy, &copy_history);
    assert(inflateEnd__dift_wrapper__(copy) == inflateEnd(&reference_copy));

    /* A failed reset must not discard the original stream's history. */
    assert(inflateReset2__dift_wrapper__(stream, 7) == inflateReset2(&reference, 7));
    finish(stream, &reference, &history);
    for (unsigned reset = 0; reset < 3; reset++) {
        if (reset == 0) {
            assert(inflateResetKeep__dift_wrapper__(stream) == inflateResetKeep(&reference));
        } else if (reset == 1) {
            assert(inflateReset__dift_wrapper__(stream) == inflateReset(&reference));
            history = 0;
        } else {
            assert(inflateReset2__dift_wrapper__(stream, -15) == inflateReset2(&reference, -15));
            history = 0;
        }
        memset(DIFT_MEM_ADDR(arena + 4096), TAG_SECRET, size);
        stream->next_in = arena + 4096;
        stream->avail_in = size;
        finish(stream, &reference, &history);
        assert(history == (reset == 0 ? TAG_ATTACKER | TAG_SECRET : TAG_SECRET));
    }
    assert(inflateEnd__dift_wrapper__(stream) == inflateEnd(&reference));

    /* Reusing the public stream object must not reuse previous tags. */
    initialize(stream, &reference, -15);
    memset(DIFT_MEM_ADDR(arena + 4096), 0, size);
    stream->next_in = arena + 4096;
    stream->avail_in = size;
    history = 0;
    finish(stream, &reference, &history);
    assert(history == 0);
    assert(inflateEnd__dift_wrapper__(stream) == inflateEnd(&reference));
}

static void independent_streams(void) {
    z_streamp first = (z_streamp)arena, second = first + 1;
    z_stream reference_first, reference_second;
    initialize(first, &reference_first, 47);
    initialize(second, &reference_second, 47);
    uInt size = compressed_input(31, NULL, 0);
    memcpy(arena + 6144, arena + 4096, size);
    memset(DIFT_MEM_ADDR(arena + 4096), TAG_ATTACKER, size);
    memset(DIFT_MEM_ADDR(arena + 6144), TAG_SECRET, size);
    first->next_in = arena + 4096;
    second->next_in = arena + 6144;
    first->avail_in = second->avail_in = size;
    dift_tag_t first_history = 0, second_history = 0;
    assert(step(first, &reference_first, Z_NO_FLUSH, 1, &first_history) == Z_OK);
    assert(step(second, &reference_second, Z_NO_FLUSH, 1, &second_history) == Z_OK);
    finish(first, &reference_first, &first_history);
    assert(first_history == TAG_ATTACKER);
    assert(inflateEnd__dift_wrapper__(first) == inflateEnd(&reference_first));
    finish(second, &reference_second, &second_history);
    assert(second_history == TAG_SECRET);
    assert(inflateEnd__dift_wrapper__(second) == inflateEnd(&reference_second));
}

static void raw_checksum(void) {
    z_streamp stream = (z_streamp)arena;
    stream->adler = 42;
    dift_set_mem_tags(&stream->adler, TAG_SECRET, sizeof(stream->adler));
    assert(inflateInit2___dift_wrapper__(stream, -15, ZLIB_VERSION, sizeof(*stream)) == Z_OK);
    assert(stream->adler == 42 && DIFT_MEM_TAG(&stream->adler) == TAG_SECRET);
    assert(inflateReset__dift_wrapper__(stream) == Z_OK);
    assert(stream->adler == 42 && DIFT_MEM_TAG(&stream->adler) == TAG_SECRET);
    assert(inflateEnd__dift_wrapper__(stream) == Z_OK);
}

static void dictionary_and_prime(void) {
    z_streamp stream = (z_streamp)arena;
    z_stream reference;
    unsigned char *dictionary = arena + 16384;
    for (unsigned i = 0; i < 256; i++)
        dictionary[i] = (unsigned char)('a' + i % 7);
    uInt size = compressed_input(15, dictionary, 256);
    initialize(stream, &reference, 15);
    memset(DIFT_MEM_ADDR(arena + 4096), TAG_ATTACKER, size);
    stream->next_in = arena + 4096;
    stream->avail_in = size;
    dift_tag_t history = 0;
    assert(step(stream, &reference, Z_NO_FLUSH, 1, &history) == Z_NEED_DICT);
    dictionary[0] ^= 1;
    memset(DIFT_MEM_ADDR(dictionary), TAG_SECRET_INDIRECT, 256);
    assert(inflateSetDictionary__dift_wrapper__(stream, dictionary, 256) == Z_DATA_ERROR);
    assert(inflateSetDictionary(&reference, dictionary, 256) == Z_DATA_ERROR);
    assert(dift_reg_tags[DIFT_RET] == (history | TAG_SECRET_INDIRECT));
    dictionary[0] ^= 1;
    memset(DIFT_MEM_ADDR(dictionary), TAG_SECRET, 256);
    assert(inflateSetDictionary__dift_wrapper__(stream, dictionary, 256) == Z_OK);
    assert(inflateSetDictionary(&reference, dictionary, 256) == Z_OK);
    history |= TAG_SECRET;
    finish(stream, &reference, &history);
    assert(history == (TAG_ATTACKER | TAG_SECRET));
    assert(inflateEnd__dift_wrapper__(stream) == inflateEnd(&reference));

    size = compressed_input(-15, NULL, 0);
    initialize(stream, &reference, -15);
    memset(DIFT_MEM_ADDR(arena + 4096), TAG_ATTACKER, size);
    dift_reg_tags[DIFT_ARG2] = TAG_SECRET_INDIRECT;
    assert(inflatePrime__dift_wrapper__(stream, 17, 0) == inflatePrime(&reference, 17, 0));
    dift_reg_tags[DIFT_ARG2] = TAG_SECRET;
    assert(inflatePrime__dift_wrapper__(stream, 8, arena[4096]) == Z_OK);
    assert(inflatePrime(&reference, 8, arena[4096]) == Z_OK);
    stream->next_in = arena + 4097;
    stream->avail_in = size - 1;
    history = TAG_SECRET;
    finish(stream, &reference, &history);
    assert(history == (TAG_ATTACKER | TAG_SECRET));
    assert(inflateEnd__dift_wrapper__(stream) == inflateEnd(&reference));
}

static void errors_and_sync(void) {
    z_streamp stream = (z_streamp)arena;
    z_stream reference;
    assert(inflateInit___dift_wrapper__(stream, "invalid", sizeof(*stream)) == Z_VERSION_ERROR);
    assert(inflateInit2___dift_wrapper__(stream, 7, ZLIB_VERSION, sizeof(*stream)) == Z_STREAM_ERROR);
    initialize(stream, &reference, 15);
    uInt size = compressed_input(15, NULL, 0);
    arena[4096 + size - 1] ^= 1;
    memset(DIFT_MEM_ADDR(arena + 4096), TAG_ATTACKER, size);
    stream->next_in = arena + 4096;
    stream->avail_in = size;
    dift_tag_t history = 0;
    assert(step(stream, &reference, Z_NO_FLUSH, 4096, &history) == Z_DATA_ERROR);
    assert(stream->total_out == 4096 && history == TAG_ATTACKER);
    assert(inflateEnd__dift_wrapper__(stream) == inflateEnd(&reference));

    initialize(stream, &reference, -15);
    size = compressed_input(-15, NULL, 0);
    memmove(arena + 4101, arena + 4096, size);
    memcpy(arena + 4096, "\x12\x00\x00\xff\xff", 5);
    memset(DIFT_MEM_ADDR(arena + 4096), TAG_ATTACKER, 5);
    memset(DIFT_MEM_ADDR(arena + 4101), TAG_SECRET, size);
    stream->next_in = reference.next_in = arena + 4096;
    stream->avail_in = reference.avail_in = 2;
    assert(inflateSync__dift_wrapper__(stream) == inflateSync(&reference));
    assert(dift_reg_tags[DIFT_RET] == TAG_ATTACKER);
    stream->avail_in = reference.avail_in = size + 3;
    assert(inflateSync__dift_wrapper__(stream) == Z_OK);
    assert(inflateSync(&reference) == Z_OK);
    assert(stream->avail_in == size && dift_reg_tags[DIFT_RET] == TAG_ATTACKER);
    history = TAG_ATTACKER;
    finish(stream, &reference, &history);
    assert(history == (TAG_ATTACKER | TAG_SECRET));
    assert(inflateEnd__dift_wrapper__(stream) == inflateEnd(&reference));
}

static void missing_lifecycle(void) {
    z_streamp stream = (z_streamp)arena;
    z_stream reference = {0};
    assert(inflateInit(stream) == Z_OK);
    assert(inflateInit(&reference) == Z_OK);
    uInt size = compressed_input(15, NULL, 0);
    stream->next_in = arena + 4096;
    stream->avail_in = size;
    dift_tag_t history = UINT8_MAX;
    finish(stream, &reference, &history);
    assert(inflateEnd__dift_wrapper__(stream) == inflateEnd(&reference));
}

static void boundaries(void) {
    z_streamp stream = (z_streamp)arena;
    unsigned char *input = arena + 4096, *output = arena + 12272;
    const unsigned char plain[16] = "page tail";
    uLongf size = 128;
    assert(compress2(input, &size, plain, sizeof(plain), Z_BEST_COMPRESSION) == Z_OK);
    memmove(input - size, input, size);
    assert(inflateInit___dift_wrapper__(stream, ZLIB_VERSION, sizeof(*stream)) == Z_OK);
    stream->next_in = input - size;
    stream->avail_in = size;
    stream->next_out = output;
    stream->avail_out = sizeof(plain);
    memset(DIFT_MEM_ADDR(input - size), TAG_ATTACKER, size);
    memset(DIFT_MEM_ADDR(output - 1), 0x40, sizeof(plain) + 1);
    assert(mprotect(DIFT_MEM_ADDR(input), 4096, PROT_NONE) == 0);
    assert(mprotect(DIFT_MEM_ADDR(output + sizeof(plain)), 4096, PROT_NONE) == 0);
    assert(inflate__dift_wrapper__(stream, Z_FINISH) == Z_STREAM_END);
    assert(memcmp(output, plain, sizeof(plain)) == 0);
    for (unsigned i = 0; i < sizeof(plain); i++)
        assert(DIFT_MEM_TAG(output + i) == TAG_ATTACKER);
    assert(DIFT_MEM_TAG(output - 1) == 0x40);
    assert(inflate__dift_wrapper__(stream, Z_NO_FLUSH) == Z_STREAM_END);
    assert(inflateEnd__dift_wrapper__(stream) == Z_OK);
    assert(mprotect(DIFT_MEM_ADDR(input), 4096, PROT_READ | PROT_WRITE) == 0);
    assert(mprotect(DIFT_MEM_ADDR(output + sizeof(plain)), 4096, PROT_READ | PROT_WRITE) == 0);
}

static void allocation_failure(void) {
    z_streamp stream = (z_streamp)arena;
    assert(inflateInit(stream) == Z_OK);
    stream->next_out = arena + 8192;
    stream->avail_out = 1;
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct rlimit limit = {0, 0};
        assert(setrlimit(RLIMIT_CORE, &limit) == 0);
        fail_allocation = 1;
        inflate__dift_wrapper__(stream, Z_NO_FLUSH);
        _exit(2);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(inflateEnd(stream) == Z_OK);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    arena = mmap((void *)0x10000000, arena_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(arena == (void *)0x10000000);
    void *shadow = mmap(DIFT_MEM_ADDR(arena), arena_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(shadow == DIFT_MEM_ADDR(arena));
    if (strcmp(argv[1], "range") == 0) output_range();
    else if (strcmp(argv[1], "streaming") == 0) streaming();
    else if (strcmp(argv[1], "lifetime") == 0) lifetime();
    else if (strcmp(argv[1], "independent") == 0) independent_streams();
    else if (strcmp(argv[1], "raw-checksum") == 0) raw_checksum();
    else if (strcmp(argv[1], "dictionary") == 0) dictionary_and_prime();
    else if (strcmp(argv[1], "errors") == 0) errors_and_sync();
    else if (strcmp(argv[1], "missing") == 0) missing_lifecycle();
    else if (strcmp(argv[1], "boundaries") == 0) boundaries();
    else if (strcmp(argv[1], "allocation-failure") == 0) allocation_failure();
    else return 2;
    assert(allocations == 0);
    assert(munmap(shadow, arena_size) == 0);
    assert(munmap(arena, arena_size) == 0);
    return 0;
}
