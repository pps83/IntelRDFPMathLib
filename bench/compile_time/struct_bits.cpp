#include <string>
#include <stdint.h>

// The BID64 bits of a decimal64_t; code that computes converts with boost::decimal::from_bid_d64 and to_bid_d64.
struct Decimal64
{
    uint64_t bits;
};

struct Order
{
    std::string symbol;
    Decimal64 quantity;
    Decimal64 filled;
};

size_t symbolLength(const Order& order)
{
    return order.symbol.size();
}
