// Replaces LLVM's dfsan/dfsan_platform.h (absent from third_party/llvm-dfsan):
// DFSan's label shadow is Teapot's tag shadow.
#ifndef DFSAN_PLATFORM_H
#define DFSAN_PLATFORM_H

#include "sanitizer_common/sanitizer_common.h"

extern "C" {
#include "dift_support.h"
}

#define MEM_TO_SHADOW(mem) ((__sanitizer::uptr)DIFT_MEM_ADDR(mem))
// Teapot has no origin shadow. No bridged wrapper tracks origins.
#define MEM_TO_ORIGIN(mem) ((__sanitizer::uptr)0)

#endif  // DFSAN_PLATFORM_H
