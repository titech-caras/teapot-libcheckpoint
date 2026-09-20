#include "config.h"

typedef unsigned char aligned_type[64] __attribute__((aligned(64)));

#if defined(TEST_BAD_SIZE)
static unsigned char value LIBCHECKPOINT_PROTECTED_SECTION;
#elif defined(TEST_BAD_ALIGNMENT)
static unsigned char value[8] LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(1);
#elif defined(TEST_REDUCED_ALIGNMENT)
static aligned_type value LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
#else
static unsigned long long value LIBCHECKPOINT_PROTECTED_SECTION;
static unsigned char aligned_value[16] LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(16);
static aligned_type naturally_aligned_value LIBCHECKPOINT_PROTECTED_SECTION;
LIBCHECKPOINT_ASSERT_PROTECTED(aligned_value);
LIBCHECKPOINT_ASSERT_PROTECTED(naturally_aligned_value);
#endif

LIBCHECKPOINT_ASSERT_PROTECTED(value);
