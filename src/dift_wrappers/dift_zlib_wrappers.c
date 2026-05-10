#include "dift_support.h"

#include <zlib.h>

#define DIFT_WRAPPER(function_name, return_type, ...) return_type function_name##__dift_wrapper__(__VA_ARGS__)

DIFT_WRAPPER(inflate, int, z_streamp stream, int flush) {
    int result = inflate(stream, flush);
    dift_set_mem_tags(stream->next_out, DIFT_MEM_TAG(stream->next_in), stream->avail_out);
    dift_copy_mem_tags(&stream->next_out, &stream->next_in, sizeof(void*));
    dift_copy_mem_tags(&stream->avail_out, &stream->avail_in, sizeof(stream->avail_in));
    return result;
}
