#include "dift_support.h"

#include <math.h>

#define DIFT_WRAPPER(function_name, return_type, ...) return_type function_name##__dift_wrapper__(__VA_ARGS__)

DIFT_WRAPPER(log2, double, double arg) {
    // TODO: automate transformation of purely functional functions
    // TODO: tag the floating-point return register.
    return log2(arg);
}
