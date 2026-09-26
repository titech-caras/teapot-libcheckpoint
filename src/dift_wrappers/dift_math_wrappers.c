#include "dift_support.h"

#include <math.h>

DIFT_WRAPPER(log2, double, double arg) {
    // TODO: automate transformation of purely functional functions
    // TODO: tag the floating-point return register.
    return log2(arg);
}
