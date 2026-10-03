#pragma once

/*
 * The contract between Teapot's rewritten code and this runtime.
 *
 * cmake/RuntimeContract.cmake compiles a probe against these headers at
 * configure time and writes, next to each archive, <archive>.contract.json:
 * the ABI facts both sides depend on (layout offsets, sizes, encodings, the
 * DIFT layout and tag storage), the capabilities of that archive and
 * information that is the runtime's alone. Teapot loads that file, compares
 * every ABI fact with what it emits and puts a module record into each
 * rewritten module.
 *
 * Records. Each archive carries one runtime record in section
 * libcheckpoint_contract, labelled __libcheckpoint_contract_v<version>_<fingerprint>.
 * Each rewritten module carries one module record in section teapot_contract
 * whose anchor field holds the address of that label, so linking an archive
 * with another ABI fails with an undefined reference. A note in
 * .note.teapot.contract refers to the module record, which keeps it under
 * --gc-sections (linkers keep allocated notes).
 *
 * Every ABI fact both sides use is compared for equality, including slots of
 * the AArch64 shadow stack and runtime-internal report regions that only need
 * to stay apart: moving one is a contract change.
 *
 * Conventions that are not numbers (report-call clobbers, the checkpoint entry
 * and rollback protocols) are covered by the version: change one, and bump
 * LIBCHECKPOINT_CONTRACT_VERSION here and in Teapot.
 */

#define LIBCHECKPOINT_CONTRACT_VERSION 1
/* The bytes "TPCT" in a little-endian word. */
#define LIBCHECKPOINT_CONTRACT_MAGIC 0x54435054
#define LIBCHECKPOINT_CONTRACT_KIND_RUNTIME 1
#define LIBCHECKPOINT_CONTRACT_KIND_MODULE 2
#define LIBCHECKPOINT_CONTRACT_HEADER_SIZE 40

/* A runtime record lists what its archive provides; a module record lists
 * what the rewritten module requires. */
#define LIBCHECKPOINT_CAPABILITY_NESTED 0x1
#define LIBCHECKPOINT_CAPABILITY_AARCH64_BTI_PAC 0x2
#define LIBCHECKPOINT_CAPABILITY_DIFT_RUNTIME 0x4
/* How much x64 vector state the runtime saves when a rewrite asks for it: at
 * least the full state (TEAPOT_X64_VECTOR_STATE auto or full, so every site
 * gets what it asks for), SSE or AVX. xmm0-7 is always available. */
#define LIBCHECKPOINT_CAPABILITY_X64_VECTOR_FULL 0x8
#define LIBCHECKPOINT_CAPABILITY_COVERAGE 0x10
#define LIBCHECKPOINT_CAPABILITY_RISCV64_FLOAT_STATE 0x20
#define LIBCHECKPOINT_CAPABILITY_X64_VECTOR_SSE 0x40
#define LIBCHECKPOINT_CAPABILITY_X64_VECTOR_AVX 0x80
#define LIBCHECKPOINT_CAPABILITIES_KNOWN 0xff

#ifdef ENABLE_NESTED_SPECULATION
#define LIBCHECKPOINT_PROVIDES_NESTED LIBCHECKPOINT_CAPABILITY_NESTED
#else
#define LIBCHECKPOINT_PROVIDES_NESTED 0
#endif
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
#define LIBCHECKPOINT_PROVIDES_AARCH64_BTI_PAC LIBCHECKPOINT_CAPABILITY_AARCH64_BTI_PAC
#else
#define LIBCHECKPOINT_PROVIDES_AARCH64_BTI_PAC 0
#endif
#ifndef DISABLE_DIFT_RUNTIME
#define LIBCHECKPOINT_PROVIDES_DIFT_RUNTIME LIBCHECKPOINT_CAPABILITY_DIFT_RUNTIME
#else
#define LIBCHECKPOINT_PROVIDES_DIFT_RUNTIME 0
#endif
/* A forced TEAPOT_X64_VECTOR_STATE overrides what the rewrite asks for:
 * xmm0-7 (1), sse (2), avx (3) or full (4); 0 honours the rewrite. */
#define LIBCHECKPOINT_X64_VECTOR_ALL \
    (LIBCHECKPOINT_CAPABILITY_X64_VECTOR_FULL | LIBCHECKPOINT_CAPABILITY_X64_VECTOR_SSE | \
     LIBCHECKPOINT_CAPABILITY_X64_VECTOR_AVX)
#if !defined(__x86_64__) || !defined(TEAPOT_X64_VECTOR_MODE) || \
    TEAPOT_X64_VECTOR_MODE == 0 || TEAPOT_X64_VECTOR_MODE == 4
#define LIBCHECKPOINT_PROVIDES_X64_VECTOR LIBCHECKPOINT_X64_VECTOR_ALL
#elif TEAPOT_X64_VECTOR_MODE == 3
#define LIBCHECKPOINT_PROVIDES_X64_VECTOR \
    (LIBCHECKPOINT_CAPABILITY_X64_VECTOR_SSE | LIBCHECKPOINT_CAPABILITY_X64_VECTOR_AVX)
#elif TEAPOT_X64_VECTOR_MODE == 2
#define LIBCHECKPOINT_PROVIDES_X64_VECTOR LIBCHECKPOINT_CAPABILITY_X64_VECTOR_SSE
#else
#define LIBCHECKPOINT_PROVIDES_X64_VECTOR 0
#endif
#ifdef COVERAGE
#define LIBCHECKPOINT_PROVIDES_COVERAGE LIBCHECKPOINT_CAPABILITY_COVERAGE
#else
#define LIBCHECKPOINT_PROVIDES_COVERAGE 0
#endif
#if defined(__riscv) && defined(ENABLE_RISCV_FLOAT_STATE)
#define LIBCHECKPOINT_PROVIDES_RISCV64_FLOAT_STATE LIBCHECKPOINT_CAPABILITY_RISCV64_FLOAT_STATE
#else
#define LIBCHECKPOINT_PROVIDES_RISCV64_FLOAT_STATE 0
#endif

/* What the archive being compiled provides, from its own build options. */
#define LIBCHECKPOINT_RUNTIME_CAPABILITIES \
    (LIBCHECKPOINT_PROVIDES_NESTED | LIBCHECKPOINT_PROVIDES_AARCH64_BTI_PAC | \
     LIBCHECKPOINT_PROVIDES_DIFT_RUNTIME | LIBCHECKPOINT_PROVIDES_X64_VECTOR | \
     LIBCHECKPOINT_PROVIDES_COVERAGE | LIBCHECKPOINT_PROVIDES_RISCV64_FLOAT_STATE)

#ifndef __ASSEMBLER__
#include <stdint.h>

/* Followed by json_size bytes of JSON, then
 * zeros up to a multiple of eight bytes. */
struct libcheckpoint_contract_record {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t header_size;
    uint32_t json_size;
    uint64_t fingerprint;
    uint64_t capabilities;
    /* Module records: the runtime record's anchor label. Runtime records: zero. */
    const void *anchor;
};

_Static_assert(sizeof(struct libcheckpoint_contract_record) == LIBCHECKPOINT_CONTRACT_HEADER_SIZE,
               "contract record header size mismatch");
_Static_assert(sizeof(void *) == 8, "contract records assume 64-bit pointers");

/* The label on this archive's own record, beside its fingerprinted anchor. */
extern const struct libcheckpoint_contract_record libcheckpoint_runtime_contract;
#endif
