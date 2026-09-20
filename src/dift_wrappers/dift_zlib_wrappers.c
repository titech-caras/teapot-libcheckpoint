#include "dift_support.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

#define DIFT_WRAPPER(function_name, return_type, ...) return_type function_name##__dift_wrapper__(__VA_ARGS__)

/* Wrappers execute outside simulation, under the runtime's single-thread
 * contract. Keep provenance separate from zlib's opaque state and callbacks.
 * A stream-lifetime union is conservative, not byte-exact decompression DIFT. */
struct inflate_tags {
    z_streamp stream;
    struct internal_state *state;
    dift_tag_t input;
    struct inflate_tags *next;
};

static struct inflate_tags *streams LIBCHECKPOINT_PROTECTED_SECTION;
LIBCHECKPOINT_ASSERT_PROTECTED(streams);

static struct inflate_tags **stream_slot(z_streamp stream) {
    struct inflate_tags **slot = &streams;
    while (*slot && (*slot)->stream != stream)
        slot = &(*slot)->next;
    return slot;
}

static struct inflate_tags *remember_stream(z_streamp stream, dift_tag_t tag) {
    struct inflate_tags **slot = stream_slot(stream);
    if (!*slot) {
        int saved_errno = errno;
        *slot = malloc(sizeof(**slot));
        if (!*slot) {
            fputs("Teapot: cannot allocate inflate tag state\n", stderr);
            abort();
        }
        (*slot)->stream = stream;
        (*slot)->next = NULL;
        errno = saved_errno;
    }
    (*slot)->state = stream->state;
    (*slot)->input = tag;
    return *slot;
}

static struct inflate_tags *find_stream(z_streamp stream) {
    struct inflate_tags *tags = *stream_slot(stream);
    if (tags && tags->state == stream->state)
        return tags;

    int saved_errno = errno;
    fputs("Teapot: inflate stream lacks wrapped initialization; using conservative tags\n", stderr);
    errno = saved_errno;
    return remember_stream(stream, UINT8_MAX);
}

static dift_tag_t memory_tag(const void *address, size_t size) {
    const unsigned char *bytes = address;
    dift_tag_t tag = 0;
    for (size_t i = 0; i < size; i++)
        tag |= DIFT_MEM_TAG(bytes + i);
    return tag;
}

static void reset_field_tags(z_streamp stream) {
    /* Raw mode can leave adler unchanged. Clear only unconditional resets. */
    dift_set_mem_tags(&stream->total_in, 0, sizeof(stream->total_in));
    dift_set_mem_tags(&stream->total_out, 0, sizeof(stream->total_out));
    dift_set_mem_tags(&stream->msg, 0, sizeof(stream->msg));
}

static void stream_effects(z_streamp stream, const z_stream *before, struct inflate_tags *tags) {
    uInt consumed = before->avail_in - stream->avail_in;
    uInt produced = before->avail_out - stream->avail_out;
    tags->input |= memory_tag(before->next_in, consumed);
    if (produced)
        dift_set_mem_tags(before->next_out, tags->input, produced);

    /* Advancing a cursor retains its own pointer dependencies, not the other
     * buffer's pointer tag. Counts also depend on the decompression history. */
#define ADD_FIELD_TAG(field) \
    dift_set_mem_tags(&stream->field, tags->input | memory_tag(&stream->field, sizeof(stream->field)), \
                      sizeof(stream->field))
    if (consumed) {
        ADD_FIELD_TAG(next_in);
        ADD_FIELD_TAG(avail_in);
        ADD_FIELD_TAG(total_in);
    }
    if (produced) {
        ADD_FIELD_TAG(next_out);
        ADD_FIELD_TAG(avail_out);
        ADD_FIELD_TAG(total_out);
    }
    if (consumed || produced) {
        ADD_FIELD_TAG(adler);
        ADD_FIELD_TAG(data_type);
    }
#undef ADD_FIELD_TAG
    dift_reg_tags[DIFT_RET] = tags->input;
}

DIFT_WRAPPER(inflate, int, z_streamp stream, int flush) {
    z_stream before = stream ? *stream : (z_stream){0};
    int result = inflate(stream, flush);
    dift_reg_tags[DIFT_RET] = 0;
    if (stream && result != Z_STREAM_ERROR)
        stream_effects(stream, &before, find_stream(stream));
    return result;
}

DIFT_WRAPPER(inflateInit_, int, z_streamp stream, const char *version, int stream_size) {
    int result = inflateInit_(stream, version, stream_size);
    if (result == Z_OK) {
        remember_stream(stream, 0);
        reset_field_tags(stream);
        dift_set_mem_tags(&stream->state, 0, sizeof(stream->state));
    }
    dift_reg_tags[DIFT_RET] = 0;
    return result;
}

DIFT_WRAPPER(inflateInit2_, int, z_streamp stream, int window_bits, const char *version, int stream_size) {
    int result = inflateInit2_(stream, window_bits, version, stream_size);
    if (result == Z_OK) {
        remember_stream(stream, 0);
        reset_field_tags(stream);
        dift_set_mem_tags(&stream->state, 0, sizeof(stream->state));
    }
    dift_reg_tags[DIFT_RET] = 0;
    return result;
}

DIFT_WRAPPER(inflateReset, int, z_streamp stream) {
    int result = inflateReset(stream);
    if (result == Z_OK) {
        remember_stream(stream, 0);
        reset_field_tags(stream);
    }
    dift_reg_tags[DIFT_RET] = 0;
    return result;
}

DIFT_WRAPPER(inflateReset2, int, z_streamp stream, int window_bits) {
    int result = inflateReset2(stream, window_bits);
    if (result == Z_OK) {
        remember_stream(stream, 0);
        reset_field_tags(stream);
    }
    dift_reg_tags[DIFT_RET] = 0;
    return result;
}

DIFT_WRAPPER(inflateResetKeep, int, z_streamp stream) {
    int result = inflateResetKeep(stream);
    if (result == Z_OK) {
        find_stream(stream); /* The retained dictionary still carries input tags. */
        reset_field_tags(stream);
    }
    dift_reg_tags[DIFT_RET] = 0;
    return result;
}

DIFT_WRAPPER(inflateCopy, int, z_streamp dest, z_streamp source) {
    int result = inflateCopy(dest, source);
    if (result == Z_OK) {
        remember_stream(dest, find_stream(source)->input);
        dift_copy_mem_tags(dest, source, sizeof(*dest));
        dift_set_mem_tags(&dest->state, 0, sizeof(dest->state));
    }
    dift_reg_tags[DIFT_RET] = 0;
    return result;
}

DIFT_WRAPPER(inflateEnd, int, z_streamp stream) {
    int result = inflateEnd(stream);
    if (result == Z_OK) {
        struct inflate_tags **slot = stream_slot(stream);
        if (*slot) {
            struct inflate_tags *tags = *slot;
            *slot = tags->next;
            int saved_errno = errno;
            free(tags);
            errno = saved_errno;
        }
        dift_set_mem_tags(&stream->state, 0, sizeof(stream->state));
    }
    dift_reg_tags[DIFT_RET] = 0;
    return result;
}

DIFT_WRAPPER(inflateSetDictionary, int, z_streamp stream, const Bytef *dictionary, uInt length) {
    int result = inflateSetDictionary(stream, dictionary, length);
    dift_reg_tags[DIFT_RET] = 0;
    if (result == Z_OK || result == Z_DATA_ERROR) {
        struct inflate_tags *tags = find_stream(stream);
        dift_tag_t tag = memory_tag(dictionary, length);
        if (result == Z_OK)
            tags->input |= tag;
        dift_reg_tags[DIFT_RET] = tags->input | tag;
    }
    return result;
}

DIFT_WRAPPER(inflatePrime, int, z_streamp stream, int bits, int value) {
    dift_tag_t tag = dift_reg_tags[DIFT_ARG2];
    int result = inflatePrime(stream, bits, value);
    if (result == Z_OK && bits > 0)
        find_stream(stream)->input |= tag;
    dift_reg_tags[DIFT_RET] = 0;
    return result;
}

DIFT_WRAPPER(inflateSync, int, z_streamp stream) {
    z_stream before = stream ? *stream : (z_stream){0};
    int result = inflateSync(stream);
    dift_reg_tags[DIFT_RET] = 0;
    if (stream && result != Z_STREAM_ERROR)
        stream_effects(stream, &before, find_stream(stream));
    return result;
}
