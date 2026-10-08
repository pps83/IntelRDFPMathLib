// Checks parsing into integers, float, double and decimal32/64/128, decimal to binary, printing and decimal arithmetic
// against exact results. Usage: parse_check [random inputs and pairs] [seed] [examples per check]
#include <boost/decimal.hpp>
#include <double-conversion/double-to-string.h>
#include <double-conversion/string-to-double.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <deque>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct alignas(16) Bid128
{
    uint64_t w[2];
};

// The library built with DECIMAL_CALL_BY_REFERENCE=0, DECIMAL_GLOBAL_ROUNDING=0, DECIMAL_GLOBAL_EXCEPTION_FLAGS=0.
extern "C"
{
uint32_t __bid32_from_string(char*, unsigned int, unsigned int*);
uint64_t __bid64_from_string(char*, unsigned int, unsigned int*);
Bid128 __bid128_from_string(char*, unsigned int, unsigned int*);
double __bid32_to_binary64(uint32_t, unsigned int, unsigned int*);
double __bid64_to_binary64(uint64_t, unsigned int, unsigned int*);
double __bid128_to_binary64(Bid128, unsigned int, unsigned int*);
float __bid32_to_binary32(uint32_t, unsigned int, unsigned int*);
float __bid64_to_binary32(uint64_t, unsigned int, unsigned int*);
float __bid128_to_binary32(Bid128, unsigned int, unsigned int*);
void __bid32_to_string(char*, uint32_t, unsigned int*);
void __bid64_to_string(char*, uint64_t, unsigned int*);
void __bid128_to_string(char*, Bid128, unsigned int*);
uint32_t __bid32_add(uint32_t, uint32_t, unsigned int, unsigned int*);
uint32_t __bid32_sub(uint32_t, uint32_t, unsigned int, unsigned int*);
uint32_t __bid32_mul(uint32_t, uint32_t, unsigned int, unsigned int*);
uint32_t __bid32_div(uint32_t, uint32_t, unsigned int, unsigned int*);
uint64_t __bid64_add(uint64_t, uint64_t, unsigned int, unsigned int*);
uint64_t __bid64_sub(uint64_t, uint64_t, unsigned int, unsigned int*);
uint64_t __bid64_mul(uint64_t, uint64_t, unsigned int, unsigned int*);
uint64_t __bid64_div(uint64_t, uint64_t, unsigned int, unsigned int*);
Bid128 __bid128_add(Bid128, Bid128, unsigned int, unsigned int*);
Bid128 __bid128_sub(Bid128, Bid128, unsigned int, unsigned int*);
Bid128 __bid128_mul(Bid128, Bid128, unsigned int, unsigned int*);
Bid128 __bid128_div(Bid128, Bid128, unsigned int, unsigned int*);
}

namespace bd = boost::decimal;

class BigUint
{
public:
    BigUint() = default;

    explicit BigUint(uint64_t value)
    {
        for (; value != 0; value >>= 32)
            limbs.push_back(uint32_t(value));
    }

    static BigUint fromWords(uint64_t high, uint64_t low)
    {
        BigUint result;
        result.limbs = {uint32_t(low), uint32_t(low >> 32), uint32_t(high), uint32_t(high >> 32)};
        result.trim();
        return result;
    }

    static BigUint fromDigits(std::string_view digits)
    {
        BigUint result;
        for (size_t i = 0; i < digits.size(); i += 9)
        {
            const size_t count = std::min<size_t>(9, digits.size() - i);
            uint32_t chunk = 0;
            uint32_t scale = 1;
            for (size_t j = 0; j < count; ++j)
            {
                chunk = chunk * 10 + uint32_t(digits[i + j] - '0');
                scale *= 10;
            }
            result.mulAdd(scale, chunk);
        }
        return result;
    }

    int bitLength() const
    {
        return limbs.empty() ? 0 : int(32 * (limbs.size() - 1)) + std::bit_width(limbs.back());
    }

    uint64_t word64(size_t index) const
    {
        const uint64_t low = 2 * index < limbs.size() ? limbs[2 * index] : 0;
        const uint64_t high = 2 * index + 1 < limbs.size() ? limbs[2 * index + 1] : 0;
        return (high << 32) | low;
    }

    void mulAdd(uint32_t factor, uint32_t addend)
    {
        uint64_t carry = addend;
        for (uint32_t& limb : limbs)
        {
            carry += uint64_t(limb) * factor;
            limb = uint32_t(carry);
            carry >>= 32;
        }
        if (carry != 0)
            limbs.push_back(uint32_t(carry));
    }

    void mulPow10(int count)
    {
        for (; count >= 9; count -= 9)
            mulAdd(1000000000, 0);
        mulAdd(kPow10[count], 0);
    }

    void mulPow5(int count)
    {
        for (; count >= 13; count -= 13)
            mulAdd(1220703125, 0);
        uint32_t factor = 1;
        for (int i = 0; i < count; ++i)
            factor *= 5;
        mulAdd(factor, 0);
    }

    void shiftLeft(int bits)
    {
        if (limbs.empty())
            return;
        const int shift = bits % 32;
        if (shift != 0)
        {
            uint32_t carry = 0;
            for (uint32_t& limb : limbs)
            {
                const uint32_t next = limb >> (32 - shift);
                limb = (limb << shift) | carry;
                carry = next;
            }
            if (carry != 0)
                limbs.push_back(carry);
        }
        limbs.insert(limbs.begin(), size_t(bits / 32), 0);
    }

    // Requires *this >= other.
    void subtract(const BigUint& other)
    {
        int64_t borrow = 0;
        for (size_t i = 0; i < limbs.size(); ++i)
        {
            const int64_t difference = int64_t(limbs[i]) - (i < other.limbs.size() ? other.limbs[i] : 0) - borrow;
            borrow = difference < 0 ? 1 : 0;
            limbs[i] = uint32_t(difference + (borrow << 32));
        }
        trim();
    }

    void add(const BigUint& other)
    {
        if (limbs.size() < other.limbs.size())
            limbs.resize(other.limbs.size(), 0);
        uint64_t carry = 0;
        for (size_t i = 0; i < limbs.size(); ++i)
        {
            carry += uint64_t(limbs[i]) + (i < other.limbs.size() ? other.limbs[i] : 0);
            limbs[i] = uint32_t(carry);
            carry >>= 32;
        }
        if (carry != 0)
            limbs.push_back(uint32_t(carry));
    }

    friend BigUint multiply(const BigUint& a, const BigUint& b)
    {
        BigUint result;
        if (a.limbs.empty() || b.limbs.empty())
            return result;
        result.limbs.assign(a.limbs.size() + b.limbs.size(), 0);
        for (size_t i = 0; i < a.limbs.size(); ++i)
        {
            uint64_t carry = 0;
            for (size_t j = 0; j < b.limbs.size(); ++j)
            {
                carry += uint64_t(a.limbs[i]) * b.limbs[j] + result.limbs[i + j];
                result.limbs[i + j] = uint32_t(carry);
                carry >>= 32;
            }
            result.limbs[i + b.limbs.size()] = uint32_t(carry);
        }
        result.trim();
        return result;
    }

    // num / den; num is left holding the remainder.
    friend BigUint divide(BigUint& num, const BigUint& den)
    {
        BigUint quotient;
        const int top = num.bitLength() - den.bitLength();
        if (top < 0)
            return quotient;
        quotient.limbs.assign(size_t(top / 32) + 1, 0);
        BigUint shifted = den;
        shifted.shiftLeft(top);
        for (int bit = top; bit >= 0; --bit)
        {
            if (compare(num, shifted) >= 0)
            {
                num.subtract(shifted);
                quotient.limbs[size_t(bit / 32)] |= uint32_t(1) << (bit % 32);
            }
            shifted.shiftRightOne();
        }
        quotient.trim();
        return quotient;
    }

    void shiftRightOne()
    {
        uint32_t carry = 0;
        for (size_t i = limbs.size(); i-- > 0;)
        {
            const uint32_t next = limbs[i] << 31;
            limbs[i] = (limbs[i] >> 1) | carry;
            carry = next;
        }
        trim();
    }

    std::string toDigits() const
    {
        if (limbs.empty())
            return "0";
        BigUint rest = *this;
        std::string reversed;
        while (!rest.limbs.empty())
        {
            uint32_t chunk = rest.divSmall(1000000000);
            for (int i = 0; i < 9 && (chunk != 0 || !rest.limbs.empty()); ++i, chunk /= 10)
                reversed += char('0' + chunk % 10);
        }
        return std::string(reversed.rbegin(), reversed.rend());
    }

    friend int compare(const BigUint& a, const BigUint& b)
    {
        if (a.limbs.size() != b.limbs.size())
            return a.limbs.size() < b.limbs.size() ? -1 : 1;
        for (size_t i = a.limbs.size(); i-- > 0;)
        {
            if (a.limbs[i] != b.limbs[i])
                return a.limbs[i] < b.limbs[i] ? -1 : 1;
        }
        return 0;
    }

private:
    static constexpr uint32_t kPow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

    uint32_t divSmall(uint32_t divisor)
    {
        uint64_t remainder = 0;
        for (size_t i = limbs.size(); i-- > 0;)
        {
            const uint64_t current = (remainder << 32) | limbs[i];
            limbs[i] = uint32_t(current / divisor);
            remainder = current % divisor;
        }
        trim();
        return uint32_t(remainder);
    }

    void trim()
    {
        while (!limbs.empty() && limbs.back() == 0)
            limbs.pop_back();
    }

    std::vector<uint32_t> limbs;
};

// (-1)^negative * coefficient * 10^exponent; the coefficient has no leading zeros and keeps trailing ones (the cohort).
struct Exact
{
    bool negative = false;
    std::string coefficient;
    int exponent = 0;
};

// The text must be wellFormed.
static Exact parseExact(std::string_view text)
{
    Exact x;
    size_t i = 0;
    if (text[i] == '-' || text[i] == '+')
    {
        x.negative = text[i] == '-';
        ++i;
    }
    int fractionDigits = 0;
    bool fraction = false;
    for (; i < text.size() && text[i] != 'e' && text[i] != 'E'; ++i)
    {
        if (text[i] == '.')
        {
            fraction = true;
            continue;
        }
        x.coefficient += text[i];
        fractionDigits += fraction ? 1 : 0;
    }
    int exponent = 0;
    if (i < text.size())
    {
        const bool negativeExponent = text[++i] == '-';
        if (text[i] == '-' || text[i] == '+')
            ++i;
        for (; i < text.size(); ++i)
            exponent = exponent * 10 + (text[i] - '0');
        if (negativeExponent)
            exponent = -exponent;
    }
    x.exponent = exponent - fractionDigits;
    x.coefficient.erase(0, std::min(x.coefficient.find_first_not_of('0'), x.coefficient.size()));
    return x;
}

static bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

// [+-]digits[.digits][(e|E)[+-]digits], with at least one digit before the exponent.
static bool wellFormed(std::string_view text)
{
    size_t i = 0;
    if (i < text.size() && (text[i] == '-' || text[i] == '+'))
        ++i;
    size_t digits = 0;
    bool dot = false;
    for (; i < text.size() && (isDigit(text[i]) || (text[i] == '.' && !dot)); ++i)
    {
        dot = dot || text[i] == '.';
        digits += isDigit(text[i]) ? 1 : 0;
    }
    if (digits == 0)
        return false;
    if (i == text.size())
        return true;
    if (text[i] != 'e' && text[i] != 'E')
        return false;
    ++i;
    if (i < text.size() && (text[i] == '-' || text[i] == '+'))
        ++i;
    const size_t exponentStart = i;
    while (i < text.size() && isDigit(text[i]))
        ++i;
    return i == text.size() && i > exponentStart && i - exponentStart < 9;
}

// "inf", "-Inf", "+Inf", "infinity": the spellings the printers use.
static bool isInfinity(std::string_view text, bool& negative)
{
    negative = !text.empty() && text[0] == '-';
    if (!text.empty() && (text[0] == '-' || text[0] == '+'))
        text.remove_prefix(1);
    std::string lower(text);
    for (char& c : lower)
        c = char(tolower((unsigned char)c));
    return lower == "inf" || lower == "infinity";
}

struct BinaryFormat
{
    const char* name;
    int precision;
    int minExponent;  // exponent of the subnormal unit
    int maxExponent;  // exponent of the last significand bit of the largest finite value
    uint64_t infinity;
    int signShift;
    int maxMagnitude; // a value of 10^maxMagnitude or more overflows
    int minMagnitude; // a value below 10^minMagnitude rounds to zero
};

static const BinaryFormat kDouble {"double", 53, -1074, 971, UINT64_C(0x7FF0000000000000), 63, 310, -330};
static const BinaryFormat kFloat {"float", 24, -149, 104, 0x7F800000, 31, 40, -50};

// The bits of coefficient * 10^exponent rounded to nearest, ties to even.
static uint64_t roundBinary(bool negative, std::string_view coefficient, int exponent, const BinaryFormat& f)
{
    const uint64_t sign = negative ? UINT64_C(1) << f.signShift : 0;
    const int magnitude = int(coefficient.size()) + exponent;
    if (coefficient.empty() || magnitude < f.minMagnitude)
        return sign;
    if (magnitude > f.maxMagnitude)
        return sign | f.infinity;

    BigUint num = BigUint::fromDigits(coefficient);
    BigUint den(1);
    if (exponent >= 0)
        num.mulPow10(exponent);
    else
        den.mulPow10(-exponent);

    const uint64_t hiddenBit = UINT64_C(1) << (f.precision - 1);
    int e = std::max(num.bitLength() - den.bitLength() - f.precision, f.minExponent);
    for (;;)
    {
        BigUint scaledNum = num;
        BigUint scaledDen = den;
        if (e < 0)
            scaledNum.shiftLeft(-e);
        else
            scaledDen.shiftLeft(e);
        uint64_t q = divide(scaledNum, scaledDen).word64(0);
        if (q >= 2 * hiddenBit)
        {
            ++e;
            continue;
        }
        if (q < hiddenBit && e > f.minExponent)
        {
            --e;
            continue;
        }

        scaledNum.shiftLeft(1);
        const int half = compare(scaledNum, scaledDen);
        if (half > 0 || (half == 0 && (q & 1) != 0))
            ++q;
        if (q == 2 * hiddenBit)
        {
            q = hiddenBit;
            ++e;
        }
        if (e > f.maxExponent)
            return sign | f.infinity;
        if (q < hiddenBit)
            return sign | q;
        return sign | (uint64_t(e - f.minExponent + 1) << (f.precision - 1)) | (q - hiddenBit);
    }
}

struct DecimalFormat
{
    const char* name;
    int precision;
    int bias;
    int minExponent; // exponent of the smallest subnormal's unit
    int maxExponent; // exponent of the largest finite value's unit
};

static const DecimalFormat kDecimal32 {"decimal32", 7, 101, -101, 90};
static const DecimalFormat kDecimal64 {"decimal64", 16, 398, -398, 369};
static const DecimalFormat kDecimal128 {"decimal128", 34, 6176, -6176, 6111};

struct Bits128
{
    uint64_t high = 0;
    uint64_t low = 0;

    bool operator==(const Bits128&) const = default;
};

struct DecimalValue
{
    bool negative = false;
    bool infinite = false;
    bool nan = false;
    std::string coefficient = "0";
    int exponent = 0;
};

// In Intel's order: the index is the library's rounding mode argument.
static constexpr int kModeCount = 5;
static const char* const kModeNames[kModeCount] = {"nearest", "down", "up", "toward zero", "away"};
static const bd::rounding_mode kBoostModes[kModeCount] = {bd::rounding_mode::fe_dec_to_nearest,
    bd::rounding_mode::fe_dec_downward, bd::rounding_mode::fe_dec_upward, bd::rounding_mode::fe_dec_toward_zero,
    bd::rounding_mode::fe_dec_to_nearest_from_zero};

// Whether keeping the first `keep` digits rounds the magnitude up; keep is zero or negative when every digit goes.
static bool roundsUp(const std::string& digits, int keep, int mode, bool negative)
{
    const int first = keep >= 0 ? digits[size_t(keep)] - '0' : 0;
    bool sticky = keep < 0;
    for (size_t i = size_t(std::max(keep + 1, 0)); i < digits.size() && !sticky; ++i)
        sticky = digits[i] != '0';
    const bool inexact = first != 0 || sticky;
    const bool odd = keep > 0 && (digits[size_t(keep) - 1] - '0') % 2 != 0;
    switch (mode)
    {
        case 0:
            return first > 5 || (first == 5 && (sticky || odd));
        case 1:
            return negative && inexact;
        case 2:
            return !negative && inexact;
        case 3:
            return false;
        default:
            return first >= 5;
    }
}

static void incrementDigits(std::string& digits)
{
    size_t i = digits.size();
    for (; i > 0 && digits[i - 1] == '9'; --i)
        digits[i - 1] = '0';
    if (i == 0)
        digits.insert(digits.begin(), '1');
    else
        ++digits[i - 1];
}

// IEEE 754 convertFromDecimalCharacter: the string's coefficient and exponent when they fit, rounded once otherwise.
static DecimalValue roundDecimal(const Exact& x, const DecimalFormat& f, int mode)
{
    DecimalValue v;
    v.negative = x.negative;
    if (x.coefficient.empty())
    {
        v.exponent = std::clamp(x.exponent, f.minExponent, f.maxExponent);
        return v;
    }

    std::string digits = x.coefficient;
    int exponent = x.exponent;
    const int drop = std::max(int(digits.size()) - f.precision, f.minExponent - exponent);
    if (drop > 0)
    {
        const int keep = int(digits.size()) - drop;
        const bool up = roundsUp(digits, keep, mode, x.negative);
        digits.resize(size_t(std::max(keep, 0)));
        exponent += drop;
        if (up)
            incrementDigits(digits);
        if (int(digits.size()) > f.precision)
        {
            digits.pop_back();
            ++exponent;
        }
    }
    if (digits.empty())
    {
        v.exponent = exponent;
        return v;
    }

    if (exponent > f.maxExponent)
    {
        const int pad = exponent - f.maxExponent;
        if (int(digits.size()) + pad <= f.precision)
        {
            digits.append(size_t(pad), '0');
            exponent = f.maxExponent;
        }
        else
        {
            v.infinite = mode == 0 || mode == 4 || (mode == 1 && x.negative) || (mode == 2 && !x.negative);
            if (v.infinite)
                return v;
            digits.assign(size_t(f.precision), '9');
            exponent = f.maxExponent;
        }
    }
    v.coefficient = digits;
    v.exponent = exponent;
    return v;
}

static constexpr int kOperationCount = 4;
static const char* const kOperationNames[kOperationCount] = {"+", "-", "*", "/"};

static bool isZero(const DecimalValue& v)
{
    return v.coefficient == "0";
}

static BigUint scaledCoefficient(const std::string& coefficient, int zeros)
{
    BigUint result = BigUint::fromDigits(coefficient);
    result.mulPow10(zeros);
    return result;
}

// a + b exactly at exponent min(qa, qb), unsigned when zero; a far smaller operand becomes one sticky digit.
static Exact exactSum(const DecimalValue& a, const DecimalValue& b, int precision)
{
    Exact x;
    x.exponent = std::min(a.exponent, b.exponent);
    if (isZero(a) || isZero(b))
    {
        if (isZero(a) && isZero(b))
            return x;
        const DecimalValue& other = isZero(a) ? b : a;
        x.negative = other.negative;
        x.coefficient = other.coefficient + std::string(size_t(other.exponent - x.exponent), '0');
        return x;
    }

    DecimalValue big = a;
    DecimalValue small = b;
    if (b.exponent + int(b.coefficient.size()) > a.exponent + int(a.coefficient.size()))
        std::swap(big, small);
    const int top = big.exponent + int(big.coefficient.size());
    if (small.exponent + int(small.coefficient.size()) <= top - precision - 3)
    {
        small.coefficient = "1";
        small.exponent = top - precision - 4;
    }
    x.exponent = std::min(big.exponent, small.exponent);
    BigUint sum = scaledCoefficient(big.coefficient, big.exponent - x.exponent);
    BigUint other = scaledCoefficient(small.coefficient, small.exponent - x.exponent);
    x.negative = big.negative;
    if (big.negative == small.negative)
    {
        sum.add(other);
    }
    else
    {
        const int order = compare(sum, other);
        if (order == 0)
            return x;
        if (order < 0)
        {
            std::swap(sum, other);
            x.negative = small.negative;
        }
        sum.subtract(other);
    }
    x.coefficient = sum.toDigits();
    return x;
}

// a / b, b nonzero: exact at the exponent nearest qa - qb when it terminates, else its first digits and a sticky one.
static Exact exactQuotient(const DecimalValue& a, const DecimalValue& b, int precision)
{
    Exact x;
    x.negative = a.negative != b.negative;
    x.exponent = a.exponent - b.exponent;
    if (isZero(a))
        return x;

    const int scale = std::max(0, precision + 1 + int(b.coefficient.size()) - int(a.coefficient.size()));
    BigUint remainder = scaledCoefficient(a.coefficient, scale);
    const BigUint quotient = divide(remainder, BigUint::fromDigits(b.coefficient));
    std::string digits = quotient.toDigits();
    int exponent = x.exponent - scale;
    if (remainder.bitLength() != 0)
    {
        digits += '1';
        --exponent;
    }
    else
    {
        for (; exponent < x.exponent && digits.back() == '0'; ++exponent)
            digits.pop_back();
    }
    x.coefficient = digits;
    x.exponent = exponent;
    return x;
}

// IEEE 754 addition, subtraction, multiplication and division of finite decimals, with their preferred exponents.
static DecimalValue referenceOperation(int operation, const DecimalValue& a, const DecimalValue& b,
                                       const DecimalFormat& f, int mode)
{
    if (operation <= 1)
    {
        DecimalValue addend = b;
        addend.negative = operation == 1 ? !b.negative : b.negative;
        Exact x = exactSum(a, addend, f.precision);
        if (x.coefficient.empty())
            x.negative = a.negative == addend.negative ? a.negative : mode == 1;
        return roundDecimal(x, f, mode);
    }
    if (operation == 2)
    {
        Exact x;
        x.negative = a.negative != b.negative;
        x.exponent = a.exponent + b.exponent;
        if (!isZero(a) && !isZero(b))
            x.coefficient = multiply(BigUint::fromDigits(a.coefficient), BigUint::fromDigits(b.coefficient)).toDigits();
        return roundDecimal(x, f, mode);
    }
    if (isZero(b))
    {
        DecimalValue v;
        v.negative = a.negative != b.negative;
        v.nan = isZero(a);
        v.infinite = !isZero(a);
        return v;
    }
    return roundDecimal(exactQuotient(a, b, f.precision), f, mode);
}

static Bits128 encodeDecimal(const DecimalValue& v, const DecimalFormat& f)
{
    const uint64_t sign = v.negative ? 1 : 0;
    const uint64_t infinity = UINT64_C(0x7800000000000000);
    if (f.precision == 7)
    {
        if (v.infinite)
            return {0, (sign << 31) | (infinity >> 32)};
        const uint64_t c = BigUint::fromDigits(v.coefficient).word64(0);
        const uint64_t biased = uint64_t(v.exponent + f.bias);
        if (c < (UINT64_C(1) << 23))
            return {0, (sign << 31) | (biased << 23) | c};
        return {0, (sign << 31) | (UINT64_C(3) << 29) | (biased << 21) | (c & 0x1FFFFF)};
    }
    if (f.precision == 16)
    {
        if (v.infinite)
            return {0, (sign << 63) | infinity};
        const uint64_t c = BigUint::fromDigits(v.coefficient).word64(0);
        const uint64_t biased = uint64_t(v.exponent + f.bias);
        if (c < (UINT64_C(1) << 53))
            return {0, (sign << 63) | (biased << 53) | c};
        return {0, (sign << 63) | (UINT64_C(3) << 61) | (biased << 51) | (c & ((UINT64_C(1) << 51) - 1))};
    }
    if (v.infinite)
        return {(sign << 63) | infinity, 0};
    const BigUint c = BigUint::fromDigits(v.coefficient);
    return {(sign << 63) | (uint64_t(v.exponent + f.bias) << 49) | c.word64(1), c.word64(0)};
}

// A non-canonical coefficient, one above 10^precision - 1, reads as zero.
static DecimalValue decodeDecimal(Bits128 bits, const DecimalFormat& f)
{
    DecimalValue v;
    const uint64_t top = f.precision == 7 ? bits.low << 32 : f.precision == 16 ? bits.low : bits.high;
    v.negative = (top >> 63) != 0;
    const unsigned special = unsigned(top >> 58) & 0x1F;
    if (special >= 0x1E)
    {
        v.infinite = special == 0x1E;
        v.nan = special == 0x1F;
        return v;
    }

    const int exponentBits = f.precision == 7 ? 8 : f.precision == 16 ? 10 : 14;
    const bool large = ((top >> 61) & 3) == 3;
    v.exponent = int((top << (large ? 3 : 1)) >> (64 - exponentBits)) - f.bias;
    BigUint c;
    if (f.precision == 7)
        c = BigUint(large ? 0x800000 | (bits.low & 0x1FFFFF) : bits.low & 0x7FFFFF);
    else if (f.precision == 16 && large)
        c = BigUint((UINT64_C(1) << 53) | (bits.low & ((UINT64_C(1) << 51) - 1)));
    else if (f.precision == 16)
        c = BigUint(bits.low & ((UINT64_C(1) << 53) - 1));
    else if (!large)
        c = BigUint::fromWords(bits.high & ((UINT64_C(1) << 49) - 1), bits.low);
    v.coefficient = c.toDigits();
    if (int(v.coefficient.size()) > f.precision)
        v.coefficient = "0";
    return v;
}

static DecimalValue withoutTrailingZeros(DecimalValue v)
{
    if (v.infinite || v.nan)
        return v;
    const size_t last = v.coefficient.find_last_not_of('0');
    if (last == std::string::npos)
    {
        v.exponent = 0;
        return v;
    }
    v.exponent += int(v.coefficient.size() - 1 - last);
    v.coefficient.resize(last + 1);
    return v;
}

static bool sameValue(const DecimalValue& a, const DecimalValue& b)
{
    const DecimalValue x = withoutTrailingZeros(a);
    const DecimalValue y = withoutTrailingZeros(b);
    return x.negative == y.negative && x.infinite == y.infinite && x.nan == y.nan && x.coefficient == y.coefficient &&
           x.exponent == y.exponent;
}

static std::string describeDecimal(const DecimalValue& v)
{
    const std::string sign = v.negative ? "-" : "";
    if (v.nan)
        return sign + "nan";
    if (v.infinite)
        return sign + "inf";
    return sign + v.coefficient + "e" + std::to_string(v.exponent);
}

static std::string describeBinary(uint64_t bits, const BinaryFormat& f)
{
    char buf[64];
    if (f.precision == 53)
        snprintf(buf, sizeof(buf), "%.17g (%016llx)", std::bit_cast<double>(bits), (unsigned long long)bits);
    else
        snprintf(buf, sizeof(buf), "%.9g (%08x)", double(std::bit_cast<float>(uint32_t(bits))), unsigned(bits));
    return buf;
}

static uint64_t binaryOfDecimal(const DecimalValue& v, const BinaryFormat& f)
{
    if (v.infinite)
        return (v.negative ? UINT64_C(1) << f.signShift : 0) | f.infinity;
    const std::string_view coefficient = v.coefficient == "0" ? std::string_view() : std::string_view(v.coefficient);
    return roundBinary(v.negative, coefficient, v.exponent, f);
}

// What a printed value reads back as, by the exact reference; false when the text is no number.
static bool readBinary(std::string_view text, const BinaryFormat& f, uint64_t& bits)
{
    bool negative = false;
    if (isInfinity(text, negative))
    {
        bits = (negative ? UINT64_C(1) << f.signShift : 0) | f.infinity;
        return true;
    }
    if (!wellFormed(text))
        return false;
    const Exact x = parseExact(text);
    bits = roundBinary(x.negative, x.coefficient, x.exponent, f);
    return true;
}

static bool readDecimal(std::string_view text, const DecimalFormat& f, DecimalValue& v)
{
    v = DecimalValue {};
    if (isInfinity(text, v.negative))
    {
        v.infinite = true;
        return true;
    }
    if (!wellFormed(text))
        return false;
    v = roundDecimal(parseExact(text), f, 0);
    return true;
}

struct Check
{
    explicit Check(std::string checkName) : name(std::move(checkName))
    {
    }

    std::string name;
    std::atomic<long long> runs {0};
    std::atomic<long long> failures {0};
    std::mutex mutex;
    std::vector<std::pair<size_t, std::string>> examples;
};

static std::deque<Check> gChecks;
static std::vector<std::string> gInputs;
static size_t gExamples = 3;

static Check* addCheck(std::string name)
{
    return &gChecks.emplace_back(std::move(name));
}

// Counts a run; true when it failed and the input is among the first inputs that failed it, so it makes an example.
static bool failed(Check* check, size_t index, bool ok)
{
    ++check->runs;
    if (ok)
        return false;
    ++check->failures;
    std::lock_guard lock(check->mutex);
    return check->examples.size() < gExamples || index < check->examples.back().first;
}

static void addExample(Check* check, size_t index, const std::string& input, const std::string& got,
                       const std::string& expected)
{
    std::string text = "\"" + input + "\": " + got + ", expected " + expected;
    std::lock_guard lock(check->mutex);
    auto& examples = check->examples;
    examples.insert(std::upper_bound(examples.begin(), examples.end(), std::make_pair(index, text)),
                    std::make_pair(index, std::move(text)));
    if (examples.size() > gExamples)
        examples.pop_back();
}

struct BinaryChecks
{
    Check* fromChars;
    Check* strtod;
    Check* doubleConversion;
    Check* printShortest;
    Check* printMaxDigits;
    Check* printPrintf;
    Check* printDoubleConversion;
    Check* roundTrip;
};

struct IntegerChecks
{
    Check* fromChars32;
    Check* fromChars64;
    Check* fromCharsUnsigned64;
    Check* strtol;
    Check* strtoll;
    Check* strtoull;
};

struct DecimalChecks
{
    const DecimalFormat* format;
    Check* intel[kModeCount];
    Check* fromChars[kModeCount];
    Check* strtod[kModeCount];
    Check* cohort;
    Check* boostToDouble;
    Check* boostToFloat;
    Check* intelToDouble;
    Check* intelToFloat;
    Check* printGeneral;
    Check* printScientific;
    Check* printFixed;
    Check* printCohort;
    Check* printIntel;
    Check* boostRoundTrip;
    Check* intelRoundTrip;
    Check* intelOperation[kOperationCount];
    Check* boostOperation[kOperationCount];
    Check* boostOperationCohort[kOperationCount];
};

static BinaryChecks gDoubleChecks;
static BinaryChecks gFloatChecks;
static IntegerChecks gIntegerChecks;
static DecimalChecks gDecimal32Checks;
static DecimalChecks gDecimal64Checks;
static DecimalChecks gDecimal128Checks;

static BinaryChecks addBinaryChecks(const char* type, const char* strtodName, const char* printfFormat)
{
    const std::string prefix = std::string(type) + " ";
    BinaryChecks checks {};
    checks.fromChars = addCheck(prefix + "std::from_chars");
    checks.strtod = addCheck(prefix + strtodName);
    checks.doubleConversion = addCheck(prefix + "double-conversion");
    checks.printShortest = addCheck(prefix + "print std::to_chars");
    checks.printMaxDigits = addCheck(prefix + "print std::to_chars max_digits10");
    checks.printPrintf = addCheck(prefix + "print printf " + printfFormat);
    checks.printDoubleConversion = addCheck(prefix + "print double-conversion");
    checks.roundTrip = addCheck(prefix + "std::to_chars then std::from_chars");
    return checks;
}

static DecimalChecks addDecimalChecks(const DecimalFormat& f, const char* strtodName)
{
    DecimalChecks checks {};
    checks.format = &f;
    const std::string prefix = std::string(f.name) + " ";
    for (int mode = 0; mode < kModeCount; ++mode)
        checks.intel[mode] = addCheck(prefix + "Intel from_string " + kModeNames[mode]);
    for (int mode = 0; mode < kModeCount; ++mode)
        checks.fromChars[mode] = addCheck(prefix + "boost from_chars " + kModeNames[mode]);
    for (int mode = 0; mode < kModeCount; ++mode)
        checks.strtod[mode] = addCheck(prefix + "boost " + strtodName + " " + kModeNames[mode]);
    checks.cohort = addCheck(prefix + "boost from_chars cohort");
    checks.boostToDouble = addCheck(prefix + "to double boost");
    checks.intelToDouble = addCheck(prefix + "to double Intel");
    checks.boostToFloat = addCheck(prefix + "to float boost");
    checks.intelToFloat = addCheck(prefix + "to float Intel");
    checks.printGeneral = addCheck(prefix + "print boost to_chars general");
    checks.printScientific = addCheck(prefix + "print boost to_chars scientific");
    checks.printFixed = addCheck(prefix + "print boost to_chars fixed");
    checks.printCohort = addCheck(prefix + "print boost to_chars cohort");
    checks.printIntel = addCheck(prefix + "print Intel to_string");
    checks.boostRoundTrip = addCheck(prefix + "boost to_chars then from_chars");
    checks.intelRoundTrip = addCheck(prefix + "Intel to_string then from_string");
    for (int operation = 0; operation < kOperationCount; ++operation)
        checks.intelOperation[operation] = addCheck(prefix + "Intel " + kOperationNames[operation]);
    for (int operation = 0; operation < kOperationCount; ++operation)
        checks.boostOperation[operation] = addCheck(prefix + "boost " + kOperationNames[operation]);
    for (int operation = 0; operation < kOperationCount; ++operation)
        checks.boostOperationCohort[operation] = addCheck(prefix + "boost " + kOperationNames[operation] + " cohort");
    return checks;
}

template <typename T>
static uint64_t bitsOf(T value)
{
    if constexpr (sizeof(T) == 8)
        return std::bit_cast<uint64_t>(value);
    else
        return std::bit_cast<uint32_t>(value);
}

static const double_conversion::StringToDoubleConverter kDoubleConversion(
    double_conversion::StringToDoubleConverter::NO_FLAGS, 0.0, NAN, "inf", "nan");

// std::from_chars reports overflow, and a nonzero value rounding to zero, as out of range and leaves the value alone.
static bool outOfRange(uint64_t expected, const BinaryFormat& f, const Exact& x)
{
    const uint64_t magnitude = expected & ~(UINT64_C(1) << f.signShift);
    return magnitude == f.infinity || (magnitude == 0 && !x.coefficient.empty());
}

static const double_conversion::DoubleToStringConverter kShortest(
    double_conversion::DoubleToStringConverter::NO_FLAGS, "inf", "nan", 'e', -6, 21, 0, 0);

static void checkPrinted(Check* check, size_t index, std::string_view printed, uint64_t bits, const BinaryFormat& f)
{
    uint64_t read = 0;
    const bool readable = readBinary(printed, f, read);
    if (failed(check, index, readable && read == bits))
    {
        const std::string readAs = readable ? describeBinary(read, f) : "?";
        addExample(check, index, describeBinary(bits, f), "\"" + std::string(printed) + "\" reads as " + readAs,
                   describeBinary(bits, f));
    }
}

// Each printer's output, read back exactly, must give the same bits.
template <typename T>
static void checkPrintBinary(const BinaryChecks& checks, const BinaryFormat& f, size_t index, uint64_t bits)
{
    T value;
    if constexpr (sizeof(T) == 8)
        value = std::bit_cast<double>(bits);
    else
        value = std::bit_cast<float>(uint32_t(bits));
    constexpr int maxDigits = std::numeric_limits<T>::max_digits10;
    char buf[64];

    auto r = std::to_chars(buf, buf + sizeof(buf), value);
    const std::string shortest(buf, r.ptr);
    checkPrinted(checks.printShortest, index, shortest, bits, f);

    r = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::general, maxDigits);
    checkPrinted(checks.printMaxDigits, index, std::string_view(buf, size_t(r.ptr - buf)), bits, f);

    snprintf(buf, sizeof(buf), "%.*g", maxDigits, double(value));
    checkPrinted(checks.printPrintf, index, buf, bits, f);

    double_conversion::StringBuilder builder(buf, int(sizeof(buf)));
    if constexpr (sizeof(T) == 8)
        kShortest.ToShortest(value, &builder);
    else
        kShortest.ToShortestSingle(value, &builder);
    checkPrinted(checks.printDoubleConversion, index, builder.Finalize(), bits, f);

    T read {};
    const char* end = shortest.data() + shortest.size();
    const auto parsed = std::from_chars(shortest.data(), end, read);
    if (failed(checks.roundTrip, index, parsed.ec == std::errc() && parsed.ptr == end && bitsOf(read) == bits))
    {
        const std::string got = "\"" + shortest + "\" reads as " + describeBinary(bitsOf(read), f);
        addExample(checks.roundTrip, index, describeBinary(bits, f), got, describeBinary(bits, f));
    }
}

template <typename T>
static void checkBinary(const BinaryChecks& checks, const BinaryFormat& f, size_t index, const std::string& text,
                        const Exact& x)
{
    const uint64_t expected = roundBinary(x.negative, x.coefficient, x.exponent, f);
    const char* end = text.data() + text.size();

    T value {};
    const auto r = std::from_chars(text.data(), end, value);
    const bool fromCharsOk = r.ptr == end && (r.ec == std::errc() ? bitsOf(value) == expected :
                                              r.ec == std::errc::result_out_of_range && outOfRange(expected, f, x));
    if (failed(checks.fromChars, index, fromCharsOk))
    {
        addExample(checks.fromChars, index, text, r.ec == std::errc() ? describeBinary(bitsOf(value), f) : "error",
                   describeBinary(expected, f));
    }

    char* parsedEnd = nullptr;
    if constexpr (sizeof(T) == 8)
        value = strtod(text.c_str(), &parsedEnd);
    else
        value = strtof(text.c_str(), &parsedEnd);
    if (failed(checks.strtod, index, parsedEnd == end && bitsOf(value) == expected))
        addExample(checks.strtod, index, text, describeBinary(bitsOf(value), f), describeBinary(expected, f));

    int processed = 0;
    if constexpr (sizeof(T) == 8)
        value = kDoubleConversion.StringToDouble(text.data(), int(text.size()), &processed);
    else
        value = kDoubleConversion.StringToFloat(text.data(), int(text.size()), &processed);
    if (failed(checks.doubleConversion, index, processed == int(text.size()) && bitsOf(value) == expected))
    {
        addExample(checks.doubleConversion, index, text, describeBinary(bitsOf(value), f),
                   describeBinary(expected, f));
    }

    checkPrintBinary<T>(checks, f, index, expected);
}

template <typename T>
static std::string describeInteger(bool parsed, T value)
{
    return parsed ? std::to_string(value) : "error";
}

// The value the C conversion functions give: the magnitude negated in the result type when it fits, else the limit.
template <typename T>
static void checkIntegerParse(Check* check, size_t index, const std::string& text, const Exact& x, bool useFromChars)
{
    using Unsigned = std::make_unsigned_t<T>;
    const BigUint magnitude = BigUint::fromDigits(x.coefficient);
    const uint64_t m = magnitude.word64(0);
    const uint64_t limit = uint64_t(std::numeric_limits<T>::max()) + (std::is_signed_v<T> && x.negative ? 1 : 0);
    const bool fits = magnitude.bitLength() <= 64 && m <= limit;
    const T fitting = T(Unsigned(x.negative ? 0 - m : m));
    const char* end = text.data() + text.size();

    if (useFromChars)
    {
        T value {};
        const auto r = std::from_chars(text.data(), end, value);
        bool ok;
        if (std::is_unsigned_v<T> && x.negative)
            ok = r.ec == std::errc::invalid_argument && r.ptr == text.data();
        else if (fits)
            ok = r.ec == std::errc() && r.ptr == end && value == fitting;
        else
            ok = r.ec == std::errc::result_out_of_range && r.ptr == end;
        if (failed(check, index, ok))
        {
            const bool rejected = std::is_unsigned_v<T> && x.negative;
            addExample(check, index, text, describeInteger(r.ec == std::errc(), value),
                       rejected ? "error" : fits ? std::to_string(fitting) : "out of range");
        }
        return;
    }

    const bool toMin = std::is_signed_v<T> && x.negative;
    const T expected = fits ? fitting : toMin ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
    errno = 0;
    char* parsedEnd = nullptr;
    T value;
    if constexpr (std::is_same_v<T, long>)
        value = strtol(text.c_str(), &parsedEnd, 10);
    else if constexpr (std::is_same_v<T, long long>)
        value = strtoll(text.c_str(), &parsedEnd, 10);
    else
        value = strtoull(text.c_str(), &parsedEnd, 10);
    const bool range = errno == ERANGE;
    if (failed(check, index, parsedEnd == end && value == expected && range == !fits))
    {
        addExample(check, index, text, std::to_string(value) + (range ? " ERANGE" : ""),
                   std::to_string(expected) + (fits ? "" : " ERANGE"));
    }
}

static Bits128 intelParse(const DecimalFormat& f, const std::string& text, int mode)
{
    unsigned flags = 0;
    char* str = const_cast<char*>(text.c_str());
    if (f.precision == 7)
        return {0, __bid32_from_string(str, unsigned(mode), &flags)};
    if (f.precision == 16)
        return {0, __bid64_from_string(str, unsigned(mode), &flags)};
    const Bid128 r = __bid128_from_string(str, unsigned(mode), &flags);
    return {r.w[1], r.w[0]};
}

static std::string intelToString(const DecimalFormat& f, Bits128 bits)
{
    unsigned flags = 0;
    char buf[128];
    if (f.precision == 7)
        __bid32_to_string(buf, uint32_t(bits.low), &flags);
    else if (f.precision == 16)
        __bid64_to_string(buf, bits.low, &flags);
    else
        __bid128_to_string(buf, Bid128 {{bits.low, bits.high}}, &flags);
    return buf;
}

static double intelToDouble(const DecimalFormat& f, Bits128 bits)
{
    unsigned flags = 0;
    if (f.precision == 7)
        return __bid32_to_binary64(uint32_t(bits.low), 0, &flags);
    if (f.precision == 16)
        return __bid64_to_binary64(bits.low, 0, &flags);
    return __bid128_to_binary64(Bid128 {{bits.low, bits.high}}, 0, &flags);
}

static float intelToFloat(const DecimalFormat& f, Bits128 bits)
{
    unsigned flags = 0;
    if (f.precision == 7)
        return __bid32_to_binary32(uint32_t(bits.low), 0, &flags);
    if (f.precision == 16)
        return __bid64_to_binary32(bits.low, 0, &flags);
    return __bid128_to_binary32(Bid128 {{bits.low, bits.high}}, 0, &flags);
}

using Intel32Operation = uint32_t (*)(uint32_t, uint32_t, unsigned int, unsigned int*);
using Intel64Operation = uint64_t (*)(uint64_t, uint64_t, unsigned int, unsigned int*);
using Intel128Operation = Bid128 (*)(Bid128, Bid128, unsigned int, unsigned int*);
static const Intel32Operation kIntel32Operations[kOperationCount] = {__bid32_add, __bid32_sub, __bid32_mul,
                                                                     __bid32_div};
static const Intel64Operation kIntel64Operations[kOperationCount] = {__bid64_add, __bid64_sub, __bid64_mul,
                                                                     __bid64_div};
static const Intel128Operation kIntel128Operations[kOperationCount] = {__bid128_add, __bid128_sub, __bid128_mul,
                                                                       __bid128_div};

static Bits128 intelOperation(const DecimalFormat& f, int operation, Bits128 a, Bits128 b, int mode)
{
    unsigned flags = 0;
    if (f.precision == 7)
        return {0, kIntel32Operations[operation](uint32_t(a.low), uint32_t(b.low), unsigned(mode), &flags)};
    if (f.precision == 16)
        return {0, kIntel64Operations[operation](a.low, b.low, unsigned(mode), &flags)};
    const Bid128 r = kIntel128Operations[operation](Bid128 {{a.low, a.high}}, Bid128 {{b.low, b.high}}, unsigned(mode),
                                                     &flags);
    return {r.w[1], r.w[0]};
}

template <typename Decimal>
static Decimal boostOperation(int operation, Decimal a, Decimal b)
{
    switch (operation)
    {
        case 0:
            return a + b;
        case 1:
            return a - b;
        case 2:
            return a * b;
        default:
            return a / b;
    }
}

static Bits128 toBits(bd::decimal32_t value)
{
    return {0, bd::to_bid_d32(value)};
}

static Bits128 toBits(bd::decimal64_t value)
{
    return {0, bd::to_bid_d64(value)};
}

static Bits128 toBits(bd::decimal128_t value)
{
    const auto bits = bd::to_bid_d128(value);
    return {bits.high, bits.low};
}

static void fromBits(Bits128 bits, bd::decimal32_t& value)
{
    value = bd::from_bid_d32(uint32_t(bits.low));
}

static void fromBits(Bits128 bits, bd::decimal64_t& value)
{
    value = bd::from_bid_d64(bits.low);
}

static void fromBits(Bits128 bits, bd::decimal128_t& value)
{
    value = bd::from_bid_d128(boost::int128::uint128_t {bits.high, bits.low});
}

static void boostStrtod(const char* str, char** end, bd::decimal32_t& value)
{
    value = bd::strtod32(str, end);
}

static void boostStrtod(const char* str, char** end, bd::decimal64_t& value)
{
    value = bd::strtod64(str, end);
}

static void boostStrtod(const char* str, char** end, bd::decimal128_t& value)
{
    value = bd::strtod128(str, end);
}

template <typename Decimal>
static void checkToBinary(const DecimalChecks& checks, size_t index, const DecimalValue& v, Bits128 bits)
{
    const DecimalFormat& f = *checks.format;
    Decimal value {};
    fromBits(bits, value);
    const std::string label = describeDecimal(v);

    const uint64_t expectedDouble = binaryOfDecimal(v, kDouble);
    const uint64_t boostDouble = bitsOf(static_cast<double>(value));
    if (failed(checks.boostToDouble, index, boostDouble == expectedDouble))
    {
        addExample(checks.boostToDouble, index, label, describeBinary(boostDouble, kDouble),
                   describeBinary(expectedDouble, kDouble));
    }
    const uint64_t intelDouble = bitsOf(intelToDouble(f, bits));
    if (failed(checks.intelToDouble, index, intelDouble == expectedDouble))
    {
        addExample(checks.intelToDouble, index, label, describeBinary(intelDouble, kDouble),
                   describeBinary(expectedDouble, kDouble));
    }

    const uint64_t expectedFloat = binaryOfDecimal(v, kFloat);
    const uint64_t boostFloat = bitsOf(static_cast<float>(value));
    if (failed(checks.boostToFloat, index, boostFloat == expectedFloat))
    {
        addExample(checks.boostToFloat, index, label, describeBinary(boostFloat, kFloat),
                   describeBinary(expectedFloat, kFloat));
    }
    const uint64_t intelFloat = bitsOf(intelToFloat(f, bits));
    if (failed(checks.intelToFloat, index, intelFloat == expectedFloat))
    {
        addExample(checks.intelToFloat, index, label, describeBinary(intelFloat, kFloat),
                   describeBinary(expectedFloat, kFloat));
    }
}

// The text read back exactly must give the value, or with withCohort the very bits.
static void checkPrintedDecimal(Check* check, size_t index, const std::string& printed, const DecimalValue& v,
                                Bits128 bits, const DecimalFormat& f, bool withCohort)
{
    DecimalValue read;
    const bool readable = readDecimal(printed, f, read);
    const bool ok = readable && (withCohort ? encodeDecimal(read, f) == bits : sameValue(read, v));
    if (failed(check, index, ok))
    {
        const std::string got = "\"" + printed + "\" reads as " + (readable ? describeDecimal(read) : "?");
        addExample(check, index, describeDecimal(v), got, describeDecimal(v));
    }
}

template <typename Decimal>
static void checkPrintDecimal(const DecimalChecks& checks, size_t index, const DecimalValue& v, Bits128 bits)
{
    const DecimalFormat& f = *checks.format;
    Decimal value {};
    fromBits(bits, value);
    char buf[7000];

    const std::pair<Check*, bd::chars_format> printers[] = {{checks.printGeneral, bd::chars_format::general},
        {checks.printScientific, bd::chars_format::scientific}, {checks.printFixed, bd::chars_format::fixed},
        {checks.printCohort, bd::chars_format::cohort_preserving_scientific}};
    std::string general;
    for (const auto& [check, format] : printers)
    {
        const auto r = bd::to_chars(buf, buf + sizeof(buf), value, format);
        const std::string printed = r.ec == std::errc() ? std::string(buf, r.ptr) : "error";
        const bool withCohort = format == bd::chars_format::cohort_preserving_scientific;
        checkPrintedDecimal(check, index, printed, v, bits, f, withCohort);
        if (format == bd::chars_format::general)
            general = printed;
    }

    const std::string intel = intelToString(f, bits);
    checkPrintedDecimal(checks.printIntel, index, intel, v, bits, f, true);

    Decimal read {};
    const auto r = bd::from_chars(general.data(), general.data() + general.size(), read);
    const DecimalValue boostRead = decodeDecimal(toBits(read), f);
    if (failed(checks.boostRoundTrip, index, r.ec == std::errc() && sameValue(boostRead, v)))
    {
        const std::string got = "\"" + general + "\" reads as " + describeDecimal(boostRead);
        addExample(checks.boostRoundTrip, index, describeDecimal(v), got, describeDecimal(v));
    }

    const Bits128 intelRead = intelParse(f, intel, 0);
    if (failed(checks.intelRoundTrip, index, intelRead == bits))
    {
        const std::string got = "\"" + intel + "\" reads as " + describeDecimal(decodeDecimal(intelRead, f));
        addExample(checks.intelRoundTrip, index, describeDecimal(v), got, describeDecimal(v));
    }
}

// boost's from_chars keeps the cohort only in its cohort preserving formats; the others are checked by value.
template <typename Decimal>
static void checkDecimal(const DecimalChecks& checks, size_t index, const std::string& text, const Exact& x, int mode)
{
    const DecimalFormat& f = *checks.format;
    const DecimalValue expected = roundDecimal(x, f, mode);
    const Bits128 expectedBits = encodeDecimal(expected, f);
    const char* end = text.data() + text.size();

    const Bits128 intel = intelParse(f, text, mode);
    if (failed(checks.intel[mode], index, intel == expectedBits))
    {
        addExample(checks.intel[mode], index, text, describeDecimal(decodeDecimal(intel, f)),
                   describeDecimal(expected));
    }

    Decimal value {};
    const auto r = bd::from_chars(text.data(), end, value);
    const bool parsed = r.ec == std::errc() && r.ptr == end;
    const DecimalValue fromChars = decodeDecimal(toBits(value), f);
    if (failed(checks.fromChars[mode], index, parsed && sameValue(fromChars, expected)))
    {
        addExample(checks.fromChars[mode], index, text, parsed ? describeDecimal(fromChars) : "error",
                   describeDecimal(expected));
    }

    char* parsedEnd = nullptr;
    boostStrtod(text.c_str(), &parsedEnd, value);
    const DecimalValue strtod = decodeDecimal(toBits(value), f);
    if (failed(checks.strtod[mode], index, parsedEnd == end && sameValue(strtod, expected)))
        addExample(checks.strtod[mode], index, text, describeDecimal(strtod), describeDecimal(expected));

    if (mode != 0)
        return;

    const bool exact = int(x.coefficient.size()) <= f.precision && x.exponent >= f.minExponent &&
                       x.exponent <= f.maxExponent;
    if (exact)
    {
        const bool scientific = text.find_first_of("eE") != std::string::npos;
        const auto format = scientific ? bd::chars_format::cohort_preserving_scientific :
                                         bd::chars_format::cohort_preserving_fixed;
        const auto cohort = bd::from_chars(text.data(), end, value, format);
        const bool parsedCohort = cohort.ec == std::errc() && cohort.ptr == end;
        if (failed(checks.cohort, index, parsedCohort && toBits(value) == expectedBits))
        {
            const std::string got = parsedCohort ? describeDecimal(decodeDecimal(toBits(value), f)) : "error";
            addExample(checks.cohort, index, text, got, describeDecimal(expected));
        }
    }
    checkToBinary<Decimal>(checks, index, expected, expectedBits);
    checkPrintDecimal<Decimal>(checks, index, expected, expectedBits);
}

static std::string operationLabel(const DecimalValue& a, int operation, const DecimalValue& b, int mode)
{
    return describeDecimal(a) + " " + kOperationNames[operation] + " " + describeDecimal(b) + " " + kModeNames[mode];
}

// NaN results match by being NaN; Intel's results also by their bits, cohort included, and boost's by value first.
template <typename Decimal>
static void checkArithmetic(const DecimalChecks& checks, size_t index, const DecimalValue& a, const DecimalValue& b,
                            int mode)
{
    const DecimalFormat& f = *checks.format;
    const Bits128 aBits = encodeDecimal(a, f);
    const Bits128 bBits = encodeDecimal(b, f);
    Decimal x {};
    Decimal y {};
    fromBits(aBits, x);
    fromBits(bBits, y);

    for (int operation = 0; operation < kOperationCount; ++operation)
    {
        const DecimalValue expected = referenceOperation(operation, a, b, f, mode);
        const Bits128 expectedBits = expected.nan ? Bits128 {} : encodeDecimal(expected, f);

        const Bits128 intel = intelOperation(f, operation, aBits, bBits, mode);
        const DecimalValue intelValue = decodeDecimal(intel, f);
        const bool intelOk = expected.nan ? intelValue.nan : intel == expectedBits;
        if (failed(checks.intelOperation[operation], index, intelOk))
        {
            addExample(checks.intelOperation[operation], index, operationLabel(a, operation, b, mode),
                       describeDecimal(intelValue), describeDecimal(expected));
        }

        const Bits128 boost = toBits(boostOperation(operation, x, y));
        const DecimalValue boostValue = decodeDecimal(boost, f);
        const bool boostOk = expected.nan ? boostValue.nan : sameValue(boostValue, expected);
        if (failed(checks.boostOperation[operation], index, boostOk))
        {
            addExample(checks.boostOperation[operation], index, operationLabel(a, operation, b, mode),
                       describeDecimal(boostValue), describeDecimal(expected));
        }
        if (boostOk && !expected.nan && failed(checks.boostOperationCohort[operation], index, boost == expectedBits))
        {
            addExample(checks.boostOperationCohort[operation], index, operationLabel(a, operation, b, mode),
                       describeDecimal(boostValue), describeDecimal(expected));
        }
    }
}

static std::vector<std::pair<size_t, size_t>> gPairs;

// Both operands are the inputs parsed to nearest; pairs with an infinite operand are left out.
template <typename Decimal>
static void checkPair(const DecimalChecks& checks, size_t index, const Exact& x, const Exact& y, int mode)
{
    const DecimalValue a = roundDecimal(x, *checks.format, 0);
    const DecimalValue b = roundDecimal(y, *checks.format, 0);
    if (!a.infinite && !b.infinite)
        checkArithmetic<Decimal>(checks, index, a, b, mode);
}

static void checkInput(size_t index, int mode)
{
    const std::string& text = gInputs[index];
    const Exact x = parseExact(text);
    if (mode == 0)
    {
        checkBinary<double>(gDoubleChecks, kDouble, index, text, x);
        checkBinary<float>(gFloatChecks, kFloat, index, text, x);
        if (text.find_first_of(".eE") == std::string::npos)
        {
            checkIntegerParse<int32_t>(gIntegerChecks.fromChars32, index, text, x, true);
            checkIntegerParse<int64_t>(gIntegerChecks.fromChars64, index, text, x, true);
            checkIntegerParse<uint64_t>(gIntegerChecks.fromCharsUnsigned64, index, text, x, true);
            checkIntegerParse<long>(gIntegerChecks.strtol, index, text, x, false);
            checkIntegerParse<long long>(gIntegerChecks.strtoll, index, text, x, false);
            checkIntegerParse<unsigned long long>(gIntegerChecks.strtoull, index, text, x, false);
        }
    }
    checkDecimal<bd::decimal32_t>(gDecimal32Checks, index, text, x, mode);
    checkDecimal<bd::decimal64_t>(gDecimal64Checks, index, text, x, mode);
    checkDecimal<bd::decimal128_t>(gDecimal128Checks, index, text, x, mode);
}

static void checkSlice(unsigned slice, unsigned slices, int mode)
{
    for (size_t i = slice; i < gInputs.size(); i += slices)
        checkInput(i, mode);
    for (size_t i = slice; i < gPairs.size(); i += slices)
    {
        const Exact x = parseExact(gInputs[gPairs[i].first]);
        const Exact y = parseExact(gInputs[gPairs[i].second]);
        checkPair<bd::decimal32_t>(gDecimal32Checks, i, x, y, mode);
        checkPair<bd::decimal64_t>(gDecimal64Checks, i, x, y, mode);
        checkPair<bd::decimal128_t>(gDecimal128Checks, i, x, y, mode);
    }
}

// Digit counts around the decimal precisions, 2^24, 2^53, 2^64 and neighbors, halves, 9s that carry, trailing zeros.
static const char* const kIntegerParts[] = {"", "0", "00", "1", "5", "9", "10", "99", "123", "007", "999999", "9999999",
    "99999999", "1000000", "10000000", "8388607", "8388608", "16777215", "16777216", "16777217", "123456789",
    "2147483647", "2147483648", "4294967295", "4294967296", "999999999999999", "9999999999999999", "99999999999999999",
    "1000000000000000", "10000000000000000", "1234567890123456", "12345678901234565", "12345678901234575",
    "9007199254740991", "9007199254740992", "9007199254740993", "9007199254740995", "9223372036854775807",
    "9223372036854775808", "18446744073709551615", "18446744073709551616", "10000000000000000000000",
    "100000000000000000000000", "1234567890123456789012345678901234", "12345678901234567890123456789012345",
    "9999999999999999999999999999999999", "99999999999999999999999999999999999", "1000000000000000000000000000000000",
    "123456789012345678901234567890123456789012"};

// nullptr: no dot; "": a dot with no digits after it.
static const char* const kFractionParts[] = {nullptr, "", "0", "00", "5", "05", "25", "125", "0625", "1", "01", "001",
    "4", "6", "49", "50", "51", "75", "9", "99", "999", "9999999", "99999999", "4999999", "5000001", "1000000",
    "0000001", "9999999999999999", "99999999999999999", "4999999999999999", "5000000000000001",
    "50000000000000000000000001", "49999999999999999999999999999999999", "50000000000000000000000000000000001",
    "9999999999999999999999999999999999", "99999999999999999999999999999999999",
    "3333333333333333333333333333333333333333", "1000000000000000055511151231257827021181583404541015625",
    "100000000000000005551115123125782702118158340454101562500001", "000000000000000000001",
    "00000000000000000000000000000000000000001"};

// Around the limits of float, double and the decimal types, and the powers of ten double holds exactly.
static const char* const kExponentParts[] = {nullptr, "e0", "e1", "e-1", "e2", "e-2", "e5", "e-5", "E10", "e-10",
    "e+15", "e16", "e-16", "e22", "e23", "e-22", "e-23", "e38", "e39", "e-38", "e-39", "e-45", "e-46", "e90", "e96",
    "e97", "e-95", "e-101", "e-102", "e-107", "e300", "e308", "e309", "e-307", "e-308", "e-320", "e-323", "e-324",
    "e-325", "e-330", "e369", "e384", "e385", "e-383", "e-398", "e-399", "e-415", "e6111", "e6144", "e6145", "e-6143",
    "e-6176", "e-6177", "e-6210", "e-0", "e+0005"};

static void addCuratedInputs()
{
    for (const char* sign : {"", "-"})
    {
        for (const char* integer : kIntegerParts)
        {
            for (const char* fraction : kFractionParts)
            {
                if (*integer == '\0' && (fraction == nullptr || *fraction == '\0'))
                    continue;
                for (const char* exponent : kExponentParts)
                {
                    std::string text = std::string(sign) + integer;
                    if (fraction != nullptr)
                        text += std::string(".") + fraction;
                    if (exponent != nullptr)
                        text += exponent;
                    gInputs.push_back(std::move(text));
                }
            }
        }
    }
}

static std::string scientific(bool negative, const std::string& digits, int exponent)
{
    return (negative ? "-" : "") + digits + "e" + std::to_string(exponent);
}

static void decrementDigits(std::string& digits)
{
    size_t i = digits.size();
    for (; digits[i - 1] == '0'; --i)
        digits[i - 1] = '9';
    --digits[i - 1];
}

// The exact halfway point to the next binary value up, strings just below and above it, and its first 17/20/40 digits.
static void addBinaryMidpoint(const BinaryFormat& f, uint64_t bits, bool negative)
{
    const int fractionBits = f.precision - 1;
    const int biased = int(bits >> fractionBits);
    const uint64_t fraction = bits & ((UINT64_C(1) << fractionBits) - 1);
    const uint64_t significand = biased == 0 ? fraction : fraction | (UINT64_C(1) << fractionBits);
    const int exponent = (biased == 0 ? f.minExponent : f.minExponent + biased - 1) - 1;

    BigUint midpoint(2 * significand + 1);
    int decimalExponent = 0;
    if (exponent >= 0)
    {
        midpoint.shiftLeft(exponent);
    }
    else
    {
        midpoint.mulPow5(-exponent);
        decimalExponent = exponent;
    }
    const std::string digits = midpoint.toDigits();
    gInputs.push_back(scientific(negative, digits, decimalExponent));
    gInputs.push_back(scientific(negative, digits + "1", decimalExponent - 1));
    std::string below = digits;
    decrementDigits(below);
    gInputs.push_back(scientific(negative, below, decimalExponent));
    for (size_t length : {17, 20, 40})
    {
        if (digits.size() > length)
        {
            const int cut = int(digits.size() - length);
            gInputs.push_back(scientific(negative, digits.substr(0, length), decimalExponent + cut));
        }
    }
}

static void addBinaryMidpoints(const BinaryFormat& f, std::mt19937_64& rng, int count)
{
    const int fractionBits = f.precision - 1;
    const uint64_t maxBiased = (f.infinity >> fractionBits) - 1;
    const uint64_t fractionMask = (UINT64_C(1) << fractionBits) - 1;
    const uint64_t twoToPrecision = (maxBiased / 2 + uint64_t(f.precision)) << fractionBits;
    const uint64_t largest = (maxBiased << fractionBits) | fractionMask;
    const uint64_t edges[] = {0, 1, fractionMask, UINT64_C(1) << fractionBits, largest, twoToPrecision};
    for (uint64_t bits : edges)
        addBinaryMidpoint(f, bits, false);
    for (int i = 0; i < count; ++i)
        addBinaryMidpoint(f, ((rng() % (maxBiased + 1)) << fractionBits) | (rng() & fractionMask), (rng() & 1) != 0);
}

static std::string randomDigits(std::mt19937_64& rng, int count)
{
    std::string digits;
    const bool nines = rng() % 8 == 0;
    for (int i = 0; i < count; ++i)
        digits += nines ? '9' : char('0' + rng() % 10);
    if (digits[0] == '0')
        digits[0] = '1';
    return digits;
}

// precision digits and a tail that makes a tie, or lands just below or above one, at exponents across the range.
static void addDecimalTies(const DecimalFormat& f, std::mt19937_64& rng, int count)
{
    static const char* const kTails[] = {"5", "50", "4", "6", "500000000000000000000000001",
                                         "499999999999999999999999999"};
    const uint64_t span = uint64_t(f.maxExponent - f.minExponent + 40);
    for (int i = 0; i < count; ++i)
    {
        const std::string digits = randomDigits(rng, f.precision) + kTails[rng() % std::size(kTails)];
        gInputs.push_back(scientific((rng() & 1) != 0, digits, f.minExponent - 20 + int(rng() % span)));
    }
}

// Random digits, mostly with runs of 0s and 9s mixed in, a dot anywhere or none, and an exponent from a mix of ranges.
static void addRandomInputs(std::mt19937_64& rng, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        std::string text = (rng() & 1) != 0 ? "-" : "";
        const int length = 1 + int(rng() % 40);
        const int dot = rng() % 3 == 0 ? -1 : int(rng() % uint64_t(length + 1));
        for (int d = 0; d < length; ++d)
        {
            if (d == dot)
                text += '.';
            const uint64_t pick = rng() % 20;
            text += pick < 3 ? '0' : pick < 6 ? '9' : char('0' + rng() % 10);
        }
        if (dot == length)
            text += '.';
        switch (rng() % 6)
        {
            case 0:
            case 1:
                break;
            case 2:
            case 3:
                text += "e" + std::to_string(int(rng() % 61) - 30);
                break;
            case 4:
                text += "e" + std::to_string(int(rng() % 801) - 400);
                break;
            default:
                text += "e" + std::to_string(int(rng() % 12501) - 6250);
                break;
        }
        gInputs.push_back(std::move(text));
    }
}

// An input with itself, with the next input (the curated ones next to each other differ in one part) or with any.
static void addPairs(std::mt19937_64& rng, size_t count)
{
    const size_t inputs = gInputs.size();
    for (size_t i = 0; i < count; ++i)
    {
        const size_t first = size_t(rng() % inputs);
        const uint64_t kind = rng() % 4;
        const size_t second = kind == 0 ? first : kind == 1 ? (first + 1) % inputs : size_t(rng() % inputs);
        gPairs.emplace_back(first, second);
    }
}

int main(int argc, char** argv)
{
    const size_t randomCount = argc > 1 ? size_t(strtoull(argv[1], nullptr, 10)) : 200000;
    const uint64_t seed = argc > 2 ? strtoull(argv[2], nullptr, 10) : 1;
    gExamples = argc > 3 ? size_t(strtoull(argv[3], nullptr, 10)) : 3;
    const auto start = std::chrono::steady_clock::now();

    std::mt19937_64 rng(seed);
    addCuratedInputs();
    addBinaryMidpoints(kDouble, rng, 3000);
    addBinaryMidpoints(kFloat, rng, 3000);
    addDecimalTies(kDecimal32, rng, 3000);
    addDecimalTies(kDecimal64, rng, 3000);
    addDecimalTies(kDecimal128, rng, 3000);
    addRandomInputs(rng, randomCount);
    addPairs(rng, randomCount);

    gDoubleChecks = addBinaryChecks("double", "strtod", "%.17g");
    gFloatChecks = addBinaryChecks("float", "strtof", "%.9g");
    gIntegerChecks = {addCheck("int32 std::from_chars"), addCheck("int64 std::from_chars"),
                      addCheck("uint64 std::from_chars"), addCheck("long strtol"), addCheck("long long strtoll"),
                      addCheck("unsigned long long strtoull")};
    gDecimal32Checks = addDecimalChecks(kDecimal32, "strtod32");
    gDecimal64Checks = addDecimalChecks(kDecimal64, "strtod64");
    gDecimal128Checks = addDecimalChecks(kDecimal128, "strtod128");

    const unsigned slices = std::max(1U, std::thread::hardware_concurrency());
    for (int mode = 0; mode < kModeCount; ++mode)
    {
        bd::fesetround(kBoostModes[mode]);
        std::vector<std::thread> pool;
        for (unsigned slice = 0; slice < slices; ++slice)
            pool.emplace_back(checkSlice, slice, slices, mode);
        for (std::thread& thread : pool)
            thread.join();
    }
    bd::fesetround(kBoostModes[0]);

    int failing = 0;
    for (Check& check : gChecks)
    {
        const long long failures = check.failures.load();
        printf("%-4s %-45s %9lld runs %9lld failures\n", failures == 0 ? "ok" : "FAIL", check.name.c_str(),
               check.runs.load(), failures);
        for (const auto& example : check.examples)
            printf("       %s\n", example.second.c_str());
        failing += failures != 0 ? 1 : 0;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    printf("%zu inputs, %zu pairs, seed %llu, %d of %zu checks failing, %.1f s\n", gInputs.size(), gPairs.size(),
           (unsigned long long)seed, failing, gChecks.size(), seconds);
    return failing != 0 ? 1 : 0;
}
