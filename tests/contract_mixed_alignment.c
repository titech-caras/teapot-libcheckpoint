/* Two module records off the 8-byte grid: the first carries one byte of JSON
 * and no padding, so the second starts 41 bytes in. The runtime walks records
 * by their rounded extent, so it must refuse the section rather than read the
 * second record from the wrong place. */
#include "runtime_contract.h"
#include "runtime_contract_fingerprint.h"

extern const char LIBCHECKPOINT_CONTRACT_ANCHOR[];

#define MODULE_RECORD(json) {                                   \
    .magic = LIBCHECKPOINT_CONTRACT_MAGIC,                      \
    .version = LIBCHECKPOINT_CONTRACT_VERSION,                  \
    .kind = LIBCHECKPOINT_CONTRACT_KIND_MODULE,                 \
    .header_size = LIBCHECKPOINT_CONTRACT_HEADER_SIZE,          \
    .json_size = (json),                                        \
    .fingerprint = LIBCHECKPOINT_CONTRACT_FINGERPRINT,          \
    .capabilities = 0,                                          \
    .anchor = LIBCHECKPOINT_CONTRACT_ANCHOR,                    \
}

struct __attribute__((packed)) unaligned_records {
    struct libcheckpoint_contract_record first;
    char json[1];
    struct libcheckpoint_contract_record second;
};

__attribute__((used, section("teapot_contract"), aligned(8)))
static const struct unaligned_records test_module_contracts = {
    .first = MODULE_RECORD(1), .json = "{", .second = MODULE_RECORD(0),
};
