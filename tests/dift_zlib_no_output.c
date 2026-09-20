#undef NDEBUG
#include <assert.h>
#include <zlib.h>
#include "dift_support.h"

extern int inflate__dift_wrapper__(z_streamp, int);
extern int inflateInit___dift_wrapper__(z_streamp, const char *, int);
extern int inflateEnd__dift_wrapper__(z_streamp);

dift_tag_t dift_reg_tags[DIFT_REG_TAGS_SIZE];
static int checking_inflate;

void dift_set_mem_tags(void *address, dift_tag_t tag, size_t size) {
    (void)address; (void)tag; (void)size;
    assert(!checking_inflate && "inflate without output must not set memory tags");
}

void dift_copy_mem_tags(void *dest, const void *source, size_t size) {
    (void)dest; (void)source; (void)size;
    assert(!checking_inflate && "inflate without output must not change output-field tags");
}

int main(void) {
    checking_inflate = 1;
    assert(inflate__dift_wrapper__(NULL, Z_NO_FLUSH) == Z_STREAM_ERROR);
    z_stream stream = {0};
    assert(inflate__dift_wrapper__(&stream, Z_NO_FLUSH) == Z_STREAM_ERROR);
    checking_inflate = 0;
    assert(inflateInit___dift_wrapper__(&stream, ZLIB_VERSION, sizeof(stream)) == Z_OK);
    unsigned char input = 0, output = 0x5a;
    stream.next_in = &input;
    stream.avail_in = 0;
    stream.next_out = &output;
    stream.avail_out = 1;
    checking_inflate = 1;
    assert(inflate__dift_wrapper__(&stream, Z_NO_FLUSH) == Z_BUF_ERROR);
    assert(output == 0x5a && stream.avail_out == 1);
    checking_inflate = 0;
    assert(inflateEnd__dift_wrapper__(&stream) == Z_OK);
    return 0;
}
