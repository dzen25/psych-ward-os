#include "common.h"

extern "C" void __assert_fail(const char *assertion, const char *file, int line, const char *function) {
    printf("PANIC: %s at %s:%d\n", assertion, file, line);
    while (1);
}

const char* sel4_err_str(seL4_Error err) {
    switch (err) {
        case seL4_NoError:          return "NoError";
        case seL4_InvalidArgument:  return "InvalidArgument";
        case seL4_InvalidCapability:return "InvalidCapability";
        case seL4_IllegalOperation: return "IllegalOperation";
        case seL4_RangeError:       return "RangeError";
        case seL4_AlignmentError:   return "AlignmentError";
        case seL4_FailedLookup:     return "FailedLookup";
        case seL4_TruncatedMessage: return "TruncatedMessage";
        case seL4_DeleteFirst:      return "DeleteFirst";
        case seL4_RevokeFirst:      return "RevokeFirst";
        case seL4_NotEnoughMemory:  return "NotEnoughMemory";
        default: return "Unknown";
    }
}

void check_err(seL4_Error err, const char *msg) {
    if (err != seL4_NoError) {
        printf("FATAL: %s -> %d (%s)\n", msg, err, sel4_err_str(err));
        while (1);
    }
}