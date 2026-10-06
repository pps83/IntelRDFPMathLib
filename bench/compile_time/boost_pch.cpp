#include "boost_pch.h"

size_t sum(std::string_view a, std::string_view b, char* out, size_t outSize)
{
    boost::decimal::decimal64_t x, y;
    boost::decimal::from_chars(a.data(), a.data() + a.size(), x);
    boost::decimal::from_chars(b.data(), b.data() + b.size(), y);
    return boost::decimal::to_chars(out, out + outSize, x + y).ptr - out;
}
