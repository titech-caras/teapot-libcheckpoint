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

/* Keep these hooks on their existing checkpoint.c lines: paper builds retain
 * their object bytes without overriding real diagnostic line numbers. */
#define TEAPOT_NESTED_CAPABILITY_NAME "hashed_nested",
#define TEAPOT_NESTED_POLICY_CHECK(record, runtime) do { \
    if (!((record)->capabilities & LIBCHECKPOINT_CAPABILITY_HASHED_NESTED)) \
        reject_contract("nested policy mismatch (hashed runtime)", \
                        (record)->capabilities, (runtime)->capabilities); \
} while (0)
#define TEAPOT_NESTED_SEED_INIT(argc, argv) teapot_nested_seed_init(argc, argv)
#define TEAPOT_NESTED_PRINT_SEED() \
    fprintf(stderr, "[teapot] Nested seed: %016" PRIx64 "\n", teapot_nested_seed_word)
#else
#define TEAPOT_NESTED_CAPABILITY_NAME
#define TEAPOT_NESTED_POLICY_CHECK(record, runtime)
#define TEAPOT_NESTED_SEED_INIT(argc, argv)
#define TEAPOT_NESTED_PRINT_SEED()
#endif
