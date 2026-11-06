#pragma once

#ifdef ENABLE_HASHED_NESTED
#include <stdint.h>

/* The first word is the existing four-byte branch counter. The following
 * eight bytes belong to the runtime and are never instrumented or memlogged. */
struct teapot_nested_site {
    uint16_t arch[2];
    uint32_t gen;
    uint16_t tree[2];
};
_Static_assert(sizeof(struct teapot_nested_site) == 12, "nested site stride");

/* The generation is u32; its storage occupies a whole protected granule. */
extern uint32_t teapot_nested_root_gen;
extern uint64_t teapot_nested_seed_word;
void teapot_nested_seed_init(int argc, char **argv);
#endif
