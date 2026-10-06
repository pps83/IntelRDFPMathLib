#define DECIMAL_CALL_BY_REFERENCE 0
#define DECIMAL_GLOBAL_ROUNDING 0
#define DECIMAL_GLOBAL_EXCEPTION_FLAGS 0
#include "bid_conf.h"
#include "bid_functions.h"
#include <string.h>

size_t sum(const char* a, const char* b, char* out)
{
    _IDEC_flags flags = 0;
    const BID_UINT64 x = bid64_from_string(const_cast<char*>(a), 0, &flags);
    const BID_UINT64 y = bid64_from_string(const_cast<char*>(b), 0, &flags);
    bid64_to_string(out, bid64_add(x, y, 0, &flags), &flags);
    return strlen(out);
}
