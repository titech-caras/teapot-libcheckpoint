/* A module record like the one Teapot emits into each rewritten module, for
 * tests that link the runtime with hand-written instrumentation. The contract
 * tests override its fields to check that the runtime refuses them. */
#include "runtime_contract.h"
#include "runtime_contract_fingerprint.h"

#ifndef TEST_CONTRACT_FINGERPRINT
#define TEST_CONTRACT_FINGERPRINT LIBCHECKPOINT_CONTRACT_FINGERPRINT
#endif
#ifndef TEST_CONTRACT_VERSION
#define TEST_CONTRACT_VERSION LIBCHECKPOINT_CONTRACT_VERSION
#endif
#ifndef TEST_CONTRACT_REQUIRED
#define TEST_CONTRACT_REQUIRED 0
#endif
#ifndef TEST_CONTRACT_MAGIC
#define TEST_CONTRACT_MAGIC LIBCHECKPOINT_CONTRACT_MAGIC
#endif

extern const char LIBCHECKPOINT_CONTRACT_ANCHOR[];
#ifdef TEST_CONTRACT_WRONG_ANCHOR
/* Right fingerprint, but the anchor is not this runtime's record. */
static const char wrong_anchor[8];
#define TEST_CONTRACT_ANCHOR wrong_anchor
#else
#define TEST_CONTRACT_ANCHOR LIBCHECKPOINT_CONTRACT_ANCHOR
#endif

#ifndef TEST_CONTRACT_JSON_SIZE
#define TEST_CONTRACT_JSON_SIZE 0
#endif

#define TEST_CONTRACT_RECORD {                          \
    .magic = TEST_CONTRACT_MAGIC,                       \
    .version = TEST_CONTRACT_VERSION,                   \
    .kind = LIBCHECKPOINT_CONTRACT_KIND_MODULE,         \
    .header_size = LIBCHECKPOINT_CONTRACT_HEADER_SIZE,  \
    .json_size = TEST_CONTRACT_JSON_SIZE,               \
    .fingerprint = TEST_CONTRACT_FINGERPRINT,           \
    .capabilities = TEST_CONTRACT_REQUIRED,             \
    .anchor = TEST_CONTRACT_ANCHOR,                     \
}

#ifdef TEST_CONTRACT_JSON_BYTES
/* The record and that many bytes of JSON, but not the padding that rounds the
 * record to 8 bytes: the section ends inside the record's extent. */
struct __attribute__((packed)) test_record_with_json {
    struct libcheckpoint_contract_record record;
    char json[TEST_CONTRACT_JSON_BYTES];
};
__attribute__((used, section("teapot_contract"), aligned(8)))
static const struct test_record_with_json test_module_contract = {
    .record = TEST_CONTRACT_RECORD, .json = "{",
};
#else
__attribute__((used, section("teapot_contract"), aligned(8)))
static const struct libcheckpoint_contract_record test_module_contract = TEST_CONTRACT_RECORD;
#endif
