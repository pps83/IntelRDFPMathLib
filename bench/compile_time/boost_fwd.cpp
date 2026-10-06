#include <boost/decimal/fwd.hpp>

void use(const boost::decimal::decimal64_t& value);

void pass(const boost::decimal::decimal64_t& value)
{
    use(value);
}
