#pragma once
#define TEAPOT_SHADOW_REGISTRY_CAPACITY 64
#ifndef TEAPOT_SHADOW_MAPPING_ENFORCEMENT
#define TEAPOT_SHADOW_MAPPING_ENFORCEMENT 0
#endif
#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
struct teapot_shadow_range {
    uintptr_t start, end;
    uint64_t normal_rw_dift;
};
extern struct teapot_shadow_range teapot_shadow_registry[TEAPOT_SHADOW_REGISTRY_CAPACITY];
extern uint64_t teapot_shadow_registry_count, teapot_shadow_mapping_ready;
/* Runtime-only: register an actual successful anonymous/private mapping.
 * rw_dift is true only for software DIFT, never merely protected storage. */
void teapot_shadow_register_owned(uintptr_t start, uintptr_t end, int rw_dift);
int mprotect__teapot_wrapper__(void *, size_t, int);
int pkey_mprotect__teapot_wrapper__(void *, size_t, int, int);
int munmap__teapot_wrapper__(void *, size_t);
void *mremap__teapot_wrapper__(void *, size_t, size_t, int, ...);
#endif
