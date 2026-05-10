#pragma once
#include <stdint.h>

typedef struct gadget_desc {
    uint64_t gadget_addr, access_addr, ckpt_addr;

    uint64_t gadget_type;
} gadget_desc_t;
