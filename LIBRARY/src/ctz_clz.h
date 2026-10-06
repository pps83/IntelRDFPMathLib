// bsf/bsr by default; /arch:AVX2 switches to tzcnt/lzcnt

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#include <limits.h>

#if (defined(__cplusplus) && (__cplusplus >= 202002L)) || \
    (defined(_MSVC_LANG) && (_MSVC_LANG >= 202002L))
#include <type_traits>
#define CONSTEVAL_ (std::is_constant_evaluated())
#define CONSTEXPR_ constexpr
#else
#define CONSTEXPR_
#endif

static CONSTEXPR_ __forceinline int __builtin_ctz(unsigned x)
{
#ifdef CONSTEVAL_
    if CONSTEVAL_
    {
        for (int i = 0; i < sizeof(x) * CHAR_BIT; ++i)
        {
            if ((x >> i) & 1)
                return i;
        }
        return sizeof(x) * CHAR_BIT;
    }
#endif
#if defined(_M_ARM) || defined(_M_ARM64) || defined(_M_HYBRID_X86_ARM64) || defined(_M_ARM64EC)
    return (int)_CountTrailingZeros(x);
#elif defined(__AVX2__) || defined(__BMI__)
    return (int)_tzcnt_u32(x);
#else
    unsigned long r;
    _BitScanForward(&r, x);
    return (int)r;
#endif
}

static CONSTEXPR_ __forceinline int __builtin_ctzll(unsigned long long x)
{
#ifdef CONSTEVAL_
    if CONSTEVAL_
    {
        for (int i = 0; i < sizeof(x) * CHAR_BIT; ++i)
        {
            if ((x >> i) & 1)
                return i;
        }
        return sizeof(x) * CHAR_BIT;
    }
#endif
#if defined(_M_ARM) || defined(_M_ARM64) || defined(_M_HYBRID_X86_ARM64) || defined(_M_ARM64EC)
    return (int)_CountTrailingZeros64(x);
#elif defined(_WIN64)
#if defined(__AVX2__) || defined(__BMI__)
    return (int)_tzcnt_u64(x);
#else
    unsigned long r;
    _BitScanForward64(&r, x);
    return (int)r;
#endif
#else
    int l = __builtin_ctz((unsigned)x);
    int h = __builtin_ctz((unsigned)(x >> 32)) + 32;
    return !!((unsigned)x) ? l : h;
#endif
}

static CONSTEXPR_ __forceinline int __builtin_ctzl(unsigned long x)
{
    return sizeof(x) == 8 ? __builtin_ctzll(x) : __builtin_ctz((unsigned)x);
}

static CONSTEXPR_ __forceinline int __builtin_clz(unsigned x)
{
#ifdef CONSTEVAL_
    if CONSTEVAL_
    {
        for (int i = 0; i < sizeof(x) * CHAR_BIT; ++i)
        {
            if (x >> (sizeof(x) * CHAR_BIT - 1 - i))
                return i;
        }
        return sizeof(x) * CHAR_BIT;
    }
#endif
#if defined(_M_ARM) || defined(_M_ARM64) || defined(_M_HYBRID_X86_ARM64) || defined(_M_ARM64EC)
    return (int)_CountLeadingZeros(x);
#elif defined(__AVX2__) || defined(__LZCNT__)
    return (int)_lzcnt_u32(x);
#else
    unsigned long r;
    _BitScanReverse(&r, x);
    return (int)(r ^ 31);
#endif
}

static CONSTEXPR_ __forceinline int __builtin_clzll(unsigned long long x)
{
#ifdef CONSTEVAL_
    if CONSTEVAL_
    {
        for (int i = 0; i < sizeof(x) * CHAR_BIT; ++i)
        {
            if (x >> (sizeof(x) * CHAR_BIT - 1 - i))
                return i;
        }
        return sizeof(x) * CHAR_BIT;
    }
#endif
#if defined(_M_ARM) || defined(_M_ARM64) || defined(_M_HYBRID_X86_ARM64) || defined(_M_ARM64EC)
    return (int)_CountLeadingZeros64(x);
#elif defined(_WIN64)
#if defined(__AVX2__) || defined(__LZCNT__)
    return (int)_lzcnt_u64(x);
#else
    unsigned long r;
    _BitScanReverse64(&r, x);
    return (int)(r ^ 63);
#endif
#else
    int l = __builtin_clz((unsigned)x) + 32;
    int h = __builtin_clz((unsigned)(x >> 32));
    return !!((unsigned)(x >> 32)) ? h : l;
#endif
}

static CONSTEXPR_ __forceinline int __builtin_clzl(unsigned long x)
{
    return sizeof(x) == 8 ? __builtin_clzll(x) : __builtin_clz((unsigned)x);
}
#undef CONSTEVAL_
#undef CONSTEXPR_
#endif // defined(_MSC_VER) && !defined(__clang__)
