#include <boost/decimal/decimal64_t.hpp>
#include <string>

struct Order
{
    std::string symbol;
    boost::decimal::decimal64_t quantity;
    boost::decimal::decimal64_t filled;
};

size_t symbolLength(const Order& order)
{
    return order.symbol.size();
}
