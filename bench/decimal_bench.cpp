#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>
#include <boost/decimal/bid_conversion.hpp>
#include <boost/decimal/charconv.hpp>
#include <boost/decimal/decimal64_t.hpp>
#include <double-conversion/double-to-string.h>
#include <dragonbox/dragonbox.h>
#include <algorithm>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

// The library built with DECIMAL_CALL_BY_REFERENCE=0, DECIMAL_GLOBAL_ROUNDING=0, DECIMAL_GLOBAL_EXCEPTION_FLAGS=0.
extern "C"
{
unsigned long long __bid64_add(unsigned long long, unsigned long long, unsigned int, unsigned int*);
unsigned long long __bid64_sub(unsigned long long, unsigned long long, unsigned int, unsigned int*);
unsigned long long __bid64_mul(unsigned long long, unsigned long long, unsigned int, unsigned int*);
unsigned long long __bid64_div(unsigned long long, unsigned long long, unsigned int, unsigned int*);
unsigned long long __bid64_from_string(char*, unsigned int, unsigned int*);
void __bid64_to_string(char*, unsigned long long, unsigned int*);
double __bid64_to_binary64(unsigned long long, unsigned int, unsigned int*);
unsigned long long __binary64_to_bid64(double, unsigned int, unsigned int*);
}

namespace bd = boost::decimal;

using IntelBinaryOp = unsigned long long (*)(unsigned long long, unsigned long long, unsigned int, unsigned int*);
using BoostBinaryOp = bd::decimal64_t (*)(bd::decimal64_t, bd::decimal64_t);
static constexpr size_t kCharsBuf = 64;

// Sizes, quantities, positions and volumes as a broker's API sends them: mostly whole shares, some fractional.
static const char* const kWireSizes[] = {"0", "1", "2", "3", "5", "10", "15", "25", "50", "75", "100", "150", "200",
    "250", "300", "400", "500", "750", "1000", "1001", "1500", "2500", "5000", "10000", "12345", "25000", "100000",
    "1000000", "987654321", "-1", "-5", "-100", "-200", "-1000", "0.5", "0.25", "2.5", "10.5", "0.0001", "0.001",
    "0.01", "0.123", "1.00", "99.99", "150.25", "1234.5", "1550.00", "125000.00", "123456.78"};
static constexpr size_t kSizeCount = sizeof(kWireSizes) / sizeof(kWireSizes[0]);
static constexpr size_t kBatch = 1000;

// Each side makes one call per operation: Intel's into its library, boost's into a NOINLINE wrapper of the header code.
static uint64_t intelParse(const char* str)
{
    unsigned flags = 0;
    return __bid64_from_string(const_cast<char*>(str), 0, &flags);
}

static size_t intelToString(uint64_t value, char* buf)
{
    unsigned flags = 0;
    __bid64_to_string(buf, value, &flags);
    return strlen(buf);
}

static uint64_t intelFromDouble(double value)
{
    unsigned flags = 0;
    return __binary64_to_bid64(value, 0, &flags);
}

static double intelToDouble(uint64_t value)
{
    unsigned flags = 0;
    return __bid64_to_binary64(value, 0, &flags);
}

static uint64_t intelBinary(IntelBinaryOp op, uint64_t a, uint64_t b)
{
    unsigned flags = 0;
    return op(a, b, 0, &flags);
}

static NOINLINE bd::decimal64_t boostParse(std::string_view str)
{
    bd::decimal64_t value;
    bd::from_chars(str.data(), str.data() + str.size(), value);
    return value;
}

static NOINLINE size_t boostToChars(bd::decimal64_t value, char* buf)
{
    return bd::to_chars(buf, buf + kCharsBuf, value, bd::chars_format::scientific).ptr - buf;
}

static NOINLINE bd::decimal64_t boostFromDouble(double value)
{
    return bd::decimal64_t(value);
}

// boost's conversion done differently: double-conversion finds the shortest digits, boost only encodes them.
static NOINLINE bd::decimal64_t googleFromDouble(double value)
{
    using double_conversion::DoubleToStringConverter;
    char digits[DoubleToStringConverter::kBase10MaximalLength + 1];
    bool negative = false;
    int length = 0;
    int point = 0;
    DoubleToStringConverter::DoubleToAscii(value, DoubleToStringConverter::SHORTEST, 0, digits,
                                           static_cast<int>(sizeof(digits)), &negative, &length, &point);
    uint64_t coefficient = 0;
    for (int i = 0; i < length; ++i)
        coefficient = coefficient * 10 + static_cast<uint64_t>(digits[i] - '0');
    return bd::decimal64_t(coefficient, point - length, negative);
}

// The same with Dragonbox, whose to_decimal takes finite non-zero values only.
static NOINLINE bd::decimal64_t dragonboxFromDouble(double value)
{
    if (value == 0)
        return bd::decimal64_t(0);
    const auto shortest = jkj::dragonbox::to_decimal(value);
    return bd::decimal64_t(shortest.significand, shortest.exponent, shortest.is_negative);
}

static NOINLINE double boostToDouble(bd::decimal64_t value)
{
    return static_cast<double>(value);
}

static NOINLINE bd::decimal64_t boostAdd(bd::decimal64_t a, bd::decimal64_t b)
{
    return a + b;
}

static NOINLINE bd::decimal64_t boostSub(bd::decimal64_t a, bd::decimal64_t b)
{
    return a - b;
}

static NOINLINE bd::decimal64_t boostMul(bd::decimal64_t a, bd::decimal64_t b)
{
    return a * b;
}

static NOINLINE bd::decimal64_t boostDiv(bd::decimal64_t a, bd::decimal64_t b)
{
    return a / b;
}

struct SameValueCount
{
    int compared = 0;
    int bitIdentical = 0;
    int differ = 0;
};

// boost::decimal keeps results at full precision: the value matches Intel's, the cohort and so the bits may not.
static bool sameValue(uint64_t intel, bd::decimal64_t boostValue, SameValueCount& count)
{
    ++count.compared;
    if (intel == bd::to_bid_d64(boostValue))
        ++count.bitIdentical;
    const bd::decimal64_t fromIntel = bd::from_bid_d64(intel);
    // boost 1.91's decimal == says 0E0 != 0E-398; its integer overload compares zeros correctly.
    if (fromIntel == 0 && boostValue == 0)
        return true;
    return fromIntel == boostValue;
}

static void expectSameValue(uint64_t intel, bd::decimal64_t boostValue, SameValueCount& count, const char* what)
{
    if (sameValue(intel, boostValue, count))
        return;
    ++count.differ;
    printf("different value: %s\n", what);
}

static void checkBinary(const char* lhs, char op, const char* rhs, uint64_t intel, bd::decimal64_t boostValue,
                        SameValueCount& count)
{
    char what[64];
    snprintf(what, sizeof(what), "%s %c %s", lhs, op, rhs);
    expectSameValue(intel, boostValue, count, what);
}

// Returns the number of results whose values differ; double to decimal differences are listed, not counted.
static int checkValues()
{
    SameValueCount count;
    std::string fromDoubleDiffers;
    for (const char* str : kWireSizes)
    {
        const uint64_t a = intelParse(str);
        const bd::decimal64_t x = boostParse(str);
        expectSameValue(a, x, count, str);

        const double value = intelToDouble(a);
        if (value != static_cast<double>(x))
        {
            ++count.differ;
            printf("different double: %s\n", str);
        }
        // Intel rounds the double's exact value to 16 digits, boost takes the shortest decimal reading back as it.
        if (!sameValue(intelFromDouble(value), bd::decimal64_t(value), count))
            fromDoubleDiffers += std::string(" ") + str;
        char what[64];
        snprintf(what, sizeof(what), "double-conversion of %s", str);
        expectSameValue(bd::to_bid_d64(googleFromDouble(value)), bd::decimal64_t(value), count, what);
        snprintf(what, sizeof(what), "Dragonbox of %s", str);
        expectSameValue(bd::to_bid_d64(dragonboxFromDouble(value)), bd::decimal64_t(value), count, what);

        for (const char* rhs : kWireSizes)
        {
            const uint64_t b = intelParse(rhs);
            const bd::decimal64_t y = boostParse(rhs);
            checkBinary(str, '+', rhs, intelBinary(__bid64_add, a, b), x + y, count);
            checkBinary(str, '-', rhs, intelBinary(__bid64_sub, a, b), x - y, count);
            checkBinary(str, '*', rhs, intelBinary(__bid64_mul, a, b), x * y, count);
            if (strcmp(rhs, "0") != 0)
                checkBinary(str, '/', rhs, intelBinary(__bid64_div, a, b), x / y, count);
        }
    }
    printf("results compared: %d, bit-identical: %d, different values: %d\n", count.compared, count.bitIdentical,
           count.differ);
    if (!fromDoubleDiffers.empty())
        printf("double to decimal gives a different value for:%s\n", fromDoubleDiffers.c_str());
    return count.differ;
}

struct DecimalInputs
{
    std::vector<std::string_view> strings;
    std::vector<uint64_t> intelLhs, intelRhs;
    std::vector<bd::decimal64_t> boostLhs, boostRhs;
    std::vector<double> doubles;
};

// Every size appears about equally often, in a shuffled order that is the same on every run.
static std::vector<const char*> shuffledSizes(std::mt19937& rng)
{
    std::vector<const char*> sizes;
    for (size_t i = 0; i < kBatch; ++i)
        sizes.push_back(kWireSizes[i % kSizeCount]);
    std::shuffle(sizes.begin(), sizes.end(), rng);
    return sizes;
}

static DecimalInputs makeInputs()
{
    std::mt19937 rng(1);
    const std::vector<const char*> lhsSizes = shuffledSizes(rng);
    const std::vector<const char*> rhsSizes = shuffledSizes(rng);
    DecimalInputs in;
    for (size_t i = 0; i < kBatch; ++i)
    {
        const char* lhs = lhsSizes[i];
        const char* rhs = strcmp(rhsSizes[i], "0") == 0 ? "1" : rhsSizes[i];
        in.strings.push_back(lhs);
        in.intelLhs.push_back(intelParse(lhs));
        in.intelRhs.push_back(intelParse(rhs));
        in.boostLhs.push_back(boostParse(lhs));
        in.boostRhs.push_back(boostParse(rhs));
        in.doubles.push_back(intelToDouble(in.intelLhs.back()));
    }
    return in;
}

static uint64_t parseIntel(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (std::string_view str : in.strings)
        sum += intelParse(str.data());
    return sum;
}

static uint64_t parseBoost(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (std::string_view str : in.strings)
        sum += bd::to_bid_d64(boostParse(str));
    return sum;
}

static size_t toStringIntel(const DecimalInputs& in)
{
    char buf[kCharsBuf];
    size_t total = 0;
    for (uint64_t value : in.intelLhs)
        total += intelToString(value, buf);
    return total;
}

static size_t toStringBoost(const DecimalInputs& in)
{
    char buf[kCharsBuf];
    size_t total = 0;
    for (bd::decimal64_t value : in.boostLhs)
        total += boostToChars(value, buf);
    return total;
}

static uint64_t fromDoubleIntel(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (double value : in.doubles)
        sum += intelFromDouble(value);
    return sum;
}

static uint64_t fromDoubleBoost(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (double value : in.doubles)
        sum += bd::to_bid_d64(boostFromDouble(value));
    return sum;
}

static uint64_t fromDoubleGoogle(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (double value : in.doubles)
        sum += bd::to_bid_d64(googleFromDouble(value));
    return sum;
}

static uint64_t fromDoubleDragonbox(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (double value : in.doubles)
        sum += bd::to_bid_d64(dragonboxFromDouble(value));
    return sum;
}

static double toDoubleIntel(const DecimalInputs& in)
{
    double sum = 0;
    for (uint64_t value : in.intelLhs)
        sum += intelToDouble(value);
    return sum;
}

static double toDoubleBoost(const DecimalInputs& in)
{
    double sum = 0;
    for (bd::decimal64_t value : in.boostLhs)
        sum += boostToDouble(value);
    return sum;
}

template <IntelBinaryOp op>
static uint64_t arithmeticIntel(const DecimalInputs& in)
{
    unsigned flags = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < in.intelLhs.size(); ++i)
        sum += op(in.intelLhs[i], in.intelRhs[i], 0, &flags);
    return sum;
}

template <BoostBinaryOp op>
static uint64_t arithmeticBoost(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < in.boostLhs.size(); ++i)
        sum += bd::to_bid_d64(op(in.boostLhs[i], in.boostRhs[i]));
    return sum;
}

// One table per operation, Intel first: nanobench's relative column is each speed as a share of Intel's.
static void configure(ankerl::nanobench::Bench& bench, const char* what)
{
    bench.title(what).unit("value").batch(kBatch).relative(true).warmup(20).minEpochIterations(200);
}

template <typename IntelFn, typename BoostFn>
static void compareSpeed(const char* what, IntelFn&& intelFn, BoostFn&& boostFn)
{
    ankerl::nanobench::Bench bench;
    configure(bench, what);
    bench.run("Intel BID64", intelFn);
    bench.run("boost::decimal decimal64_t", boostFn);
}

static void compareFromDouble(const DecimalInputs& in)
{
    ankerl::nanobench::Bench bench;
    configure(bench, "double to decimal");
    bench.run("Intel BID64", [&] { ankerl::nanobench::doNotOptimizeAway(fromDoubleIntel(in)); });
    bench.run("boost::decimal decimal64_t", [&] { ankerl::nanobench::doNotOptimizeAway(fromDoubleBoost(in)); });
    bench.run("double-conversion digits, decimal64_t encoding",
              [&] { ankerl::nanobench::doNotOptimizeAway(fromDoubleGoogle(in)); });
    bench.run("Dragonbox digits, decimal64_t encoding",
              [&] { ankerl::nanobench::doNotOptimizeAway(fromDoubleDragonbox(in)); });
}

int main()
{
    const int differ = checkValues();
    const DecimalInputs in = makeInputs();
    compareSpeed("string to decimal", [&] { ankerl::nanobench::doNotOptimizeAway(parseIntel(in)); },
                 [&] { ankerl::nanobench::doNotOptimizeAway(parseBoost(in)); });
    compareSpeed("decimal to string", [&] { ankerl::nanobench::doNotOptimizeAway(toStringIntel(in)); },
                 [&] { ankerl::nanobench::doNotOptimizeAway(toStringBoost(in)); });
    compareFromDouble(in);
    compareSpeed("decimal to double", [&] { ankerl::nanobench::doNotOptimizeAway(toDoubleIntel(in)); },
                 [&] { ankerl::nanobench::doNotOptimizeAway(toDoubleBoost(in)); });
    compareSpeed("+", [&] { ankerl::nanobench::doNotOptimizeAway(arithmeticIntel<__bid64_add>(in)); },
                 [&] { ankerl::nanobench::doNotOptimizeAway(arithmeticBoost<boostAdd>(in)); });
    compareSpeed("-", [&] { ankerl::nanobench::doNotOptimizeAway(arithmeticIntel<__bid64_sub>(in)); },
                 [&] { ankerl::nanobench::doNotOptimizeAway(arithmeticBoost<boostSub>(in)); });
    compareSpeed("*", [&] { ankerl::nanobench::doNotOptimizeAway(arithmeticIntel<__bid64_mul>(in)); },
                 [&] { ankerl::nanobench::doNotOptimizeAway(arithmeticBoost<boostMul>(in)); });
    compareSpeed("/", [&] { ankerl::nanobench::doNotOptimizeAway(arithmeticIntel<__bid64_div>(in)); },
                 [&] { ankerl::nanobench::doNotOptimizeAway(arithmeticBoost<boostDiv>(in)); });
    return differ == 0 ? 0 : 1;
}
