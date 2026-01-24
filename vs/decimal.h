#pragma once
// avoid redifinition warnings/errors
#define __bid128_mul __bid128_mul_rm
#define Decimal128 Decimal128_rm
#include "../EXAMPLES/decimal.h_000"
#undef __bid128_mul
#undef Decimal128
#define Decimal128 BID_UINT128
