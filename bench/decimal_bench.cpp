#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>
#include <boost/decimal/bid_conversion.hpp>
#include <boost/decimal/charconv.hpp>
#include <boost/decimal/decimal64_t.hpp>
#include <boost/decimal/float_conversion.hpp>
#include <double-conversion/double-to-string.h>
#include <dragonbox/dragonbox.h>
#include <algorithm>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include <math.h>
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
float __bid64_to_binary32(unsigned long long, unsigned int, unsigned int*);
unsigned long long __binary32_to_bid64(float, unsigned int, unsigned int*);
unsigned long long __bid64_from_int64(long long, unsigned int, unsigned int*);
unsigned long long __bid64_scalbn(unsigned long long, int, unsigned int, unsigned int*);
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

static uint64_t intelFromFloat(float value)
{
    unsigned flags = 0;
    return __binary32_to_bid64(value, 0, &flags);
}

static float intelToFloat(uint64_t value)
{
    unsigned flags = 0;
    return __bid64_to_binary32(value, 0, &flags);
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

static NOINLINE bd::decimal64_t boostFromFloat(float value)
{
    return bd::decimal64_t(value);
}

// Intel's conversion in boost::decimal (IEEE 754 convertFormat): the exact binary value rounded to 16 digits.
static NOINLINE bd::decimal64_t exactFromDouble(double value)
{
    return bd::from_binary<bd::decimal64_t>(value);
}

static NOINLINE bd::decimal64_t exactFromFloat(float value)
{
    return bd::from_binary<bd::decimal64_t>(value);
}

// boost's conversion done differently: double-conversion finds the shortest digits, boost only encodes them.
static bd::decimal64_t googleShortest(double value, double_conversion::DoubleToStringConverter::DtoaMode mode)
{
    using double_conversion::DoubleToStringConverter;
    char digits[DoubleToStringConverter::kBase10MaximalLength + 1];
    bool negative = false;
    int length = 0;
    int point = 0;
    DoubleToStringConverter::DoubleToAscii(value, mode, 0, digits, static_cast<int>(sizeof(digits)), &negative, &length,
                                           &point);
    uint64_t coefficient = 0;
    for (int i = 0; i < length; ++i)
        coefficient = coefficient * 10 + static_cast<uint64_t>(digits[i] - '0');
    return bd::decimal64_t(coefficient, point - length, negative);
}

static NOINLINE bd::decimal64_t googleFromDouble(double value)
{
    return googleShortest(value, double_conversion::DoubleToStringConverter::SHORTEST);
}

static NOINLINE bd::decimal64_t googleFromFloat(float value)
{
    return googleShortest(value, double_conversion::DoubleToStringConverter::SHORTEST_SINGLE);
}

// The same with Dragonbox, whose to_decimal takes finite non-zero values only.
template <typename Float>
static bd::decimal64_t dragonboxShortest(Float value)
{
    if (value == 0)
        return bd::decimal64_t(0);
    const auto shortest = jkj::dragonbox::to_decimal(value);
    return bd::decimal64_t(shortest.significand, shortest.exponent, shortest.is_negative);
}

static NOINLINE bd::decimal64_t dragonboxFromDouble(double value)
{
    return dragonboxShortest(value);
}

static NOINLINE bd::decimal64_t dragonboxFromFloat(float value)
{
    return dragonboxShortest(value);
}

// BDE's restoreDecimalDigits: the value printed with the given significant digits, parsed by Intel's library.
static uint64_t bdePrinted(double value, int digits)
{
    char buffer[42];
    snprintf(buffer, sizeof(buffer), "%1.*g", digits, value);
    return intelParse(buffer);
}

// BDE's DecimalConvertUtil::decimal64FromDouble(x, -1) (shortestDecimalFromBinary in bdldfp_decimalconvertutil.cpp):
// x printed with 15, then 16 significant digits, kept once it reads back as x.
static NOINLINE uint64_t bdeShortestFromDouble(double value)
{
    for (int digits = 15;; ++digits)
    {
        const uint64_t result = bdePrinted(value, digits);
        if (digits == 16 || intelToDouble(result) == value)
            return result;
    }
}

// The same for float: 6 to 9 significant digits.
static NOINLINE uint64_t bdeShortestFromFloat(float value)
{
    for (int digits = 6;; ++digits)
    {
        const uint64_t result = bdePrinted(value, digits);
        if (digits == 9 || intelToFloat(result) == value)
            return result;
    }
}

// BDE's reduce(): drops the powers of ten the scaling brought in, so .001 gives 1e-3 and not 1000e-6.
template <typename Integer>
static void bdeReduce(Integer& significand, int& exponent)
{
    while ((significand & 7) == 0 && significand % 1000 == 0 && exponent <= -3)
    {
        significand /= 1000;
        exponent += 3;
    }
    while ((significand & 1) == 0 && significand % 10 == 0 && exponent <= -1)
    {
        significand /= 10;
        ++exponent;
    }
}

// BDE's DecimalUtil::makeDecimalRaw64 on Intel's library.
static uint64_t bdeMakeDecimal(long long significand, int exponent)
{
    unsigned flags = 0;
    return __bid64_scalbn(__bid64_from_int64(significand, 0, &flags), exponent, 0, &flags);
}

// BDE's DecimalConvertUtil::decimal64FromDouble(x) (quickDecimalFromDouble in bdldfp_decimalconvertutil.cpp): x times
// 1e9 rounded to an integer, kept if the rounding was tiny or it reads back as x; else 15 digits printed and parsed.
static NOINLINE uint64_t bdeDefaultFromDouble(double value)
{
    if (value != 0 && -1e6 < value && value < 1e6)
    {
        const double scaled = value * 1e9;
        long long significand = static_cast<long long>(scaled + copysign(.5, scaled));
        int exponent = -9;
        bdeReduce(significand, exponent);
        if (significand < 1000000000000000LL && significand > -1000000000000000LL)
        {
            const uint64_t result = bdeMakeDecimal(significand, exponent);
            double whole = 0;
            const double fraction = modf(scaled, &whole);
            if ((whole != 0 && fraction / whole < 1e-17) || fraction == 0 || intelToDouble(result) == value)
                return result;
        }
    }
    return bdePrinted(value, 15);
}

// BDE's decimal64FromFloat(x) (quickDecimalFromFloat): x scaled by its decade to 6 or 7 digits and rounded the same
// way; else 7 digits printed and parsed in about [1e-3, 8.6e9], where 7-digit decimals give distinct floats, 6 outside.
static NOINLINE uint64_t bdeDefaultFromFloat(float value)
{
    const float magnitude = fabsf(value);
    if (value != 0 && -1e06f < value && value < 1e06f)
    {
        static const float kDecadeEnds[] = {1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f};
        static const float kScales[] = {1e6f, 1e5f, 1e4f, 1e3f, 1e2f, 1e1f, 1e0f};
        int decade = 0;
        while (decade < 6 && magnitude >= kDecadeEnds[decade])
            ++decade;
        const float scaled = value * kScales[decade];
        int significand = static_cast<int>(scaled + copysignf(.5f, scaled));
        const float diff = scaled - static_cast<float>(significand);
        int exponent = decade - 6;
        bdeReduce(significand, exponent);
        if (significand < 10000000 && significand > -10000000)
        {
            const uint64_t result = bdeMakeDecimal(significand, exponent);
            if (fabsf(diff / scaled) < 1e-8f || intelToFloat(result) == value)
                return result;
        }
    }
    return bdePrinted(value, magnitude >= 9.999995e-4f && magnitude <= 8.589972e+9f ? 7 : 6);
}

static NOINLINE double boostToDouble(bd::decimal64_t value)
{
    return static_cast<double>(value);
}

static NOINLINE float boostToFloat(bd::decimal64_t value)
{
    return static_cast<float>(value);
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

static void expectSameBits(uint64_t intel, bd::decimal64_t boostValue, SameValueCount& count, const char* what)
{
    ++count.compared;
    if (intel == bd::to_bid_d64(boostValue))
    {
        ++count.bitIdentical;
        return;
    }
    ++count.differ;
    printf("different bits: %s\n", what);
}

// The digits of a number as typed without its leading and trailing zeros: "0.0100" has one.
static int significantDigits(const char* str)
{
    int first = -1;
    int last = -1;
    int index = 0;
    for (const char* c = str; *c != '\0'; ++c)
    {
        if (*c < '0' || *c > '9')
            continue;
        if (*c != '0')
        {
            if (first < 0)
                first = index;
            last = index;
        }
        ++index;
    }
    return first < 0 ? 0 : last - first + 1;
}

static void checkBinary(const char* lhs, char op, const char* rhs, uint64_t intel, bd::decimal64_t boostValue,
                        SameValueCount& count)
{
    char what[64];
    snprintf(what, sizeof(what), "%s %c %s", lhs, op, rhs);
    expectSameValue(intel, boostValue, count, what);
}

// Returns the number of results whose values differ; double and float to decimal differences are listed, not counted.
static int checkValues()
{
    SameValueCount count;
    std::string fromDoubleDiffers;
    std::string fromFloatDiffers;
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
        snprintf(what, sizeof(what), "BDE shortest of %s", str);
        expectSameValue(bdeShortestFromDouble(value), bd::decimal64_t(value), count, what);
        // BDE's default restores the number the double was made from when it has 15 digits or fewer
        snprintf(what, sizeof(what), "BDE default of %s", str);
        expectSameValue(bdeDefaultFromDouble(value), x, count, what);
        snprintf(what, sizeof(what), "from_binary of %s", str);
        expectSameBits(intelFromDouble(value), exactFromDouble(value), count, what);

        const float single = intelToFloat(a);
        if (single != static_cast<float>(x))
        {
            ++count.differ;
            printf("different float: %s\n", str);
        }
        if (!sameValue(intelFromFloat(single), bd::decimal64_t(single), count))
            fromFloatDiffers += std::string(" ") + str;
        snprintf(what, sizeof(what), "double-conversion of float %s", str);
        expectSameValue(bd::to_bid_d64(googleFromFloat(single)), bd::decimal64_t(single), count, what);
        snprintf(what, sizeof(what), "Dragonbox of float %s", str);
        expectSameValue(bd::to_bid_d64(dragonboxFromFloat(single)), bd::decimal64_t(single), count, what);
        snprintf(what, sizeof(what), "from_binary of float %s", str);
        expectSameBits(intelFromFloat(single), exactFromFloat(single), count, what);
        snprintf(what, sizeof(what), "BDE shortest of float %s", str);
        expectSameValue(bdeShortestFromFloat(single), bd::decimal64_t(single), count, what);
        // BDE's default restores the number the float was made from when it has 6 digits or fewer
        if (significantDigits(str) <= 6)
        {
            snprintf(what, sizeof(what), "BDE default of float %s", str);
            expectSameValue(bdeDefaultFromFloat(single), x, count, what);
        }

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
    if (!fromFloatDiffers.empty())
        printf("float to decimal gives a different value for:%s\n", fromFloatDiffers.c_str());
    return count.differ;
}

struct DecimalInputs
{
    std::vector<std::string_view> strings;
    std::vector<uint64_t> intelLhs, intelRhs;
    std::vector<bd::decimal64_t> boostLhs, boostRhs;
    std::vector<double> doubles;
    std::vector<float> floats;
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
        in.floats.push_back(intelToFloat(in.intelLhs.back()));
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

static uint64_t fromDoubleExact(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (double value : in.doubles)
        sum += bd::to_bid_d64(exactFromDouble(value));
    return sum;
}

// static uint64_t fromDoubleGoogle(const DecimalInputs& in)
// {
//     uint64_t sum = 0;
//     for (double value : in.doubles)
//         sum += bd::to_bid_d64(googleFromDouble(value));
//     return sum;
// }

static uint64_t fromDoubleDragonbox(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (double value : in.doubles)
        sum += bd::to_bid_d64(dragonboxFromDouble(value));
    return sum;
}

static uint64_t fromDoubleBde(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (double value : in.doubles)
        sum += bdeShortestFromDouble(value);
    return sum;
}

static uint64_t fromDoubleBdeDefault(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (double value : in.doubles)
        sum += bdeDefaultFromDouble(value);
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

static uint64_t fromFloatIntel(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (float value : in.floats)
        sum += intelFromFloat(value);
    return sum;
}

static uint64_t fromFloatBoost(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (float value : in.floats)
        sum += bd::to_bid_d64(boostFromFloat(value));
    return sum;
}

static uint64_t fromFloatExact(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (float value : in.floats)
        sum += bd::to_bid_d64(exactFromFloat(value));
    return sum;
}

// static uint64_t fromFloatGoogle(const DecimalInputs& in)
// {
//     uint64_t sum = 0;
//     for (float value : in.floats)
//         sum += bd::to_bid_d64(googleFromFloat(value));
//     return sum;
// }

static uint64_t fromFloatDragonbox(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (float value : in.floats)
        sum += bd::to_bid_d64(dragonboxFromFloat(value));
    return sum;
}

static uint64_t fromFloatBde(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (float value : in.floats)
        sum += bdeShortestFromFloat(value);
    return sum;
}

static uint64_t fromFloatBdeDefault(const DecimalInputs& in)
{
    uint64_t sum = 0;
    for (float value : in.floats)
        sum += bdeDefaultFromFloat(value);
    return sum;
}

static float toFloatIntel(const DecimalInputs& in)
{
    float sum = 0;
    for (uint64_t value : in.intelLhs)
        sum += intelToFloat(value);
    return sum;
}

static float toFloatBoost(const DecimalInputs& in)
{
    float sum = 0;
    for (bd::decimal64_t value : in.boostLhs)
        sum += boostToFloat(value);
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
    bench.run("boost::decimal64_t", boostFn);
}

static void compareFromDouble(const DecimalInputs& in)
{
    ankerl::nanobench::Bench bench;
    configure(bench, "double to decimal");
    bench.run("Intel BID64", [&] { ankerl::nanobench::doNotOptimizeAway(fromDoubleIntel(in)); });
    bench.run("boost::decimal64_t", [&] { ankerl::nanobench::doNotOptimizeAway(fromDoubleBoost(in)); });
    bench.run("from_binary", [&] { ankerl::nanobench::doNotOptimizeAway(fromDoubleExact(in)); });
    // boost::decimal finds its digits with Dragonbox now, so double-conversion's row tells nothing new
    // bench.run("double-conversion", [&] { ankerl::nanobench::doNotOptimizeAway(fromDoubleGoogle(in)); });
    bench.run("Dragonbox", [&] { ankerl::nanobench::doNotOptimizeAway(fromDoubleDragonbox(in)); });
    bench.run("BDE shortest", [&] { ankerl::nanobench::doNotOptimizeAway(fromDoubleBde(in)); });
    bench.run("BDE default", [&] { ankerl::nanobench::doNotOptimizeAway(fromDoubleBdeDefault(in)); });
}

static void compareFromFloat(const DecimalInputs& in)
{
    ankerl::nanobench::Bench bench;
    configure(bench, "float to decimal");
    bench.run("Intel BID64", [&] { ankerl::nanobench::doNotOptimizeAway(fromFloatIntel(in)); });
    bench.run("boost::decimal64_t", [&] { ankerl::nanobench::doNotOptimizeAway(fromFloatBoost(in)); });
    bench.run("from_binary", [&] { ankerl::nanobench::doNotOptimizeAway(fromFloatExact(in)); });
    // bench.run("double-conversion", [&] { ankerl::nanobench::doNotOptimizeAway(fromFloatGoogle(in)); });
    bench.run("Dragonbox", [&] { ankerl::nanobench::doNotOptimizeAway(fromFloatDragonbox(in)); });
    bench.run("BDE shortest", [&] { ankerl::nanobench::doNotOptimizeAway(fromFloatBde(in)); });
    bench.run("BDE default", [&] { ankerl::nanobench::doNotOptimizeAway(fromFloatBdeDefault(in)); });
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
    compareFromFloat(in);
    compareSpeed("decimal to float", [&] { ankerl::nanobench::doNotOptimizeAway(toFloatIntel(in)); },
                 [&] { ankerl::nanobench::doNotOptimizeAway(toFloatBoost(in)); });
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
