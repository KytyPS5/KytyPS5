#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_BACKEND_SPIRV_SOFTFLOAT64_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_BACKEND_SPIRV_SOFTFLOAT64_H_

#include <cstdint>
#include <initializer_list>

// IEEE 754 binary64 arithmetic on 64-bit integers, for a host GPU without 64-bit floats. Each
// operation gives the bits the native emitter gives on a host with them. Rounding is to nearest
// even; denormals are kept.
//
// The code has no branches, only selects, so the same algorithm runs on the CPU (with plain
// integers, against native doubles in the tests) and makes SPIR-V (with value IDs). `Ops` gives:
//   types V (64-bit unsigned), W (32-bit unsigned) and B (a condition);
//   V K(uint64_t); V Add, Sub, Mul (low 64 bits), And, Or, Xor (V, V);
//   V Shl, Shr (V value, V amount): the amount is always below 64;
//   B Eq, Ult, Slt (V, V): Slt compares as signed; B AndB, OrB (B, B), NotB (B);
//   V Sel (B, V, V); V Zext (W); W Trunc (V).
// Every expression is evaluated, also where a select drops it, so none of them may be undefined:
// a shift amount is masked to below 64 before use.

namespace Libs::Graphics::ShaderRecompiler::SoftFloat64 {

template <typename Ops>
class Math {
public:
	using V = typename Ops::V;
	using W = typename Ops::W;
	using B = typename Ops::B;

	explicit Math(Ops& ops): o(ops) {}

	// The canonical NaN of an invalid operation.
	static constexpr uint64_t DEFAULT_NAN = 0x7ff8000000000000ull;

	V Add(V a, V b) { return AddSigned(a, b); }
	V Sub(V a, V b) { return AddSigned(a, o.Xor(b, K(SIGN))); }

	V Mul(V a, V b) {
		const auto ua = Unpack(a), ub = Unpack(b);
		const auto sign = o.Xor(ua.sign, ub.sign);
		const auto any_zero = o.OrB(ua.zero, ub.zero);
		const auto product = MulFull(ua.m, ub.m);
		// The product has 105 or 106 bits: keep the top 64 of it shifted up by 22, the rest jams.
		const auto sig = o.Or(o.Or(o.Shl(product.hi, K(22)), o.Shr(product.lo, K(42))),
		                      Jam(o.And(product.lo, K((uint64_t {1} << 42u) - 1u))));
		const auto q = o.Sub(o.Add(ua.e, ub.e), K(2150 - 42));
		auto result = o.Sel(any_zero, sign, RoundPack(sign, q, o.Sel(any_zero, K(1), sig)));
		const auto invalid = o.OrB(o.AndB(ua.inf, ub.zero), o.AndB(ua.zero, ub.inf));
		result = o.Sel(o.OrB(ua.inf, ub.inf), o.Or(sign, K(INF)), result);
		result = o.Sel(invalid, K(DEFAULT_NAN), result);
		return PropagateNan(result, Operand {a, ua.nan}, Operand {b, ub.nan});
	}

	V Fma(V a, V b, V c) {
		const auto ua = Unpack(a), ub = Unpack(b), uc = Unpack(c);
		const auto product_sign = o.Xor(ua.sign, ub.sign);
		const auto product_zero = o.OrB(ua.zero, ub.zero);
		const auto product = MulFull(ua.m, ub.m);
		// Both terms with their leading bit near bit 124 of 128.
		const Wide p {o.Or(o.Shl(product.hi, K(20)), o.Shr(product.lo, K(44))),
		              o.Shl(product.lo, K(20))};
		const auto p_exp = o.Sub(o.Add(ua.e, ub.e), K(2150 + 20));
		const Wide c_wide {o.Shl(uc.m, K(8)), K(0)};
		const auto c_exp = o.Sub(uc.e, K(1075 + 72));
		// A zero term does not set the exponent.
		const auto p_larger = o.NotB(o.Slt(p_exp, c_exp));
		const auto use_p = o.AndB(o.NotB(product_zero), o.OrB(uc.zero, p_larger));
		const auto top = o.Sel(use_p, p_exp, c_exp);
		const auto p_aligned = ShrJam128(p, NonNegative(o.Sub(top, p_exp)));
		const auto c_aligned = ShrJam128(c_wide, NonNegative(o.Sub(top, c_exp)));
		const auto same = o.Eq(product_sign, uc.sign);
		const auto c_bigger = Ult128(p_aligned, c_aligned);
		const auto larger = Select128(c_bigger, c_aligned, p_aligned);
		const auto smaller = Select128(c_bigger, p_aligned, c_aligned);
		const auto sum = Select128(same, Add128(p_aligned, c_aligned), Sub128(larger, smaller));
		const auto sign = o.Sel(o.AndB(o.NotB(same), c_bigger), uc.sign, product_sign);
		const auto sum_zero = o.Eq(o.Or(sum.hi, sum.lo), K(0));
		const auto lz = Clz128(sum);
		const auto normalized = Shl128(sum, o.Sel(sum_zero, K(0), lz));
		const auto sig = o.Or(normalized.hi, Jam(normalized.lo));
		const auto q = o.Sub(o.Add(top, K(64)), lz);
		auto result = RoundPack(sign, q, o.Sel(sum_zero, K(1), sig));
		// An exact zero is +0, also from cancellation, except -0 + -0.
		result = o.Sel(sum_zero, o.And(product_sign, uc.sign), result);
		const auto product_inf = o.OrB(ua.inf, ub.inf);
		const auto invalid =
		    o.OrB(o.OrB(o.AndB(ua.inf, ub.zero), o.AndB(ua.zero, ub.inf)),
		          o.AndB(o.AndB(product_inf, uc.inf), o.NotB(same)));
		result = o.Sel(uc.inf, c, result);
		result = o.Sel(product_inf, o.Or(product_sign, K(INF)), result);
		result = o.Sel(invalid, K(DEFAULT_NAN), result);
		return PropagateNan(result, Operand {a, ua.nan}, Operand {b, ub.nan}, Operand {c, uc.nan});
	}

	// 1 / x, rounded once.
	V Recip(V a) {
		const auto ua = Unpack(a);
		// Long division of 2^115 by the significand: 63 quotient bits, then the rest jams.
		auto rem = K(uint64_t {1} << 53u);
		auto quotient = K(0);
		for (int bit = 0; bit < 63; bit++) {
			const auto fits = o.NotB(o.Ult(rem, ua.m));
			quotient = o.Or(o.Shl(quotient, K(1)), o.Sel(fits, K(1), K(0)));
			rem = o.Shl(o.Sel(fits, o.Sub(rem, ua.m), rem), K(1));
		}
		const auto sig = o.Or(quotient, Jam(rem));
		auto result = RoundPack(ua.sign, o.Sub(K(960), ua.e), sig);
		result = o.Sel(ua.zero, o.Or(ua.sign, K(INF)), result);
		result = o.Sel(ua.inf, ua.sign, result);
		return PropagateNan(result, Operand {a, ua.nan});
	}

	// The native emitter's V_MIN_F64 and V_MAX_F64: the bits of the chosen operand. Equal values
	// differ only in the sign of zero: min takes -0, max +0. A NaN operand picks the other one,
	// two NaNs pick the second.
	V Min(V a, V b) { return MinMax(a, b, false); }
	V Max(V a, V b) { return MinMax(a, b, true); }

	B OrdEqual(V a, V b) { return o.AndB(Ordered(a, b), o.Eq(Key(a), Key(b))); }
	B OrdLessThanEqual(V a, V b) {
		return o.AndB(Ordered(a, b), o.NotB(o.Slt(Key(b), Key(a))));
	}
	B OrdGreaterThanEqual(V a, V b) {
		return o.AndB(Ordered(a, b), o.NotB(o.Slt(Key(a), Key(b))));
	}

	V Floor(V a) { return Round(a, RoundTo::Floor); }
	V Ceil(V a) { return Round(a, RoundTo::Ceil); }
	V Trunc(V a) { return Round(a, RoundTo::Trunc); }
	// GLSL fract: x - floor(x), rounded; -2^-1074 gives 1.0, infinity gives NaN.
	V Fract(V a) { return Sub(a, Floor(a)); }

	V FromS32(W value) {
		const auto wide = o.Zext(value);
		const auto negative = o.NotB(o.Ult(wide, K(0x80000000u)));
		const auto magnitude = o.Sel(negative, o.Sub(K(uint64_t {1} << 32u), wide), wide);
		const auto zero = o.Eq(magnitude, K(0));
		const auto sign = o.Sel(negative, K(SIGN), K(0));
		return o.Sel(zero, K(0), RoundPack(sign, K(0), o.Sel(zero, K(1), magnitude)));
	}

	V FromU32(W value) {
		const auto wide = o.Zext(value);
		const auto zero = o.Eq(wide, K(0));
		return o.Sel(zero, K(0), RoundPack(K(0), K(0), o.Sel(zero, K(1), wide)));
	}

	// The native ConvertF64F32: a denormal input is first flushed to a zero of its sign.
	V FromF32(W bits) {
		const auto wide = o.Zext(bits);
		const auto sign = o.Shl(o.Shr(wide, K(31)), K(63));
		const auto exponent = o.And(o.Shr(wide, K(23)), K(0xff));
		const auto fraction = o.Shl(o.And(wide, K(0x7fffff)), K(29));
		auto result = o.Or(sign, o.Or(o.Shl(o.Add(exponent, K(1023 - 127)), K(52)), fraction));
		const auto special = o.Eq(exponent, K(0xff));
		const auto nan = o.AndB(special, o.NotB(o.Eq(fraction, K(0))));
		result = o.Sel(special, o.Or(sign, o.Or(K(INF), fraction)), result);
		result = o.Sel(nan, o.Or(result, K(QUIET)), result);
		return o.Sel(o.Eq(exponent, K(0)), sign, result);
	}

	// The native ConvertF32F64: rounded to nearest even; a finite value from 2^128 up, before
	// rounding, gives the largest float of its sign; a denormal result is flushed to a signed zero.
	W ToF32(V a) {
		const auto ua = Unpack(a);
		const auto sign = o.Shr(ua.sign, K(32));
		const auto exponent = o.Sub(ua.e, K(1023 - 127));
		const auto subnormal = o.Slt(exponent, K(1));
		const auto shift = o.Sel(subnormal, o.Sub(K(1), exponent), K(0));
		const auto sig = ShrJam(ua.m, shift);
		const auto base = o.Sel(subnormal, K(0), o.Sub(exponent, K(1)));
		auto mantissa = o.Shr(sig, K(29));
		const auto rest = o.And(sig, K((uint64_t {1} << 29u) - 1u));
		mantissa = o.Add(mantissa, RoundUp(mantissa, rest, uint64_t {1} << 28u));
		auto result = o.Or(sign, o.Add(o.Shl(o.And(base, K(0xff)), K(23)), mantissa));
		result = o.Sel(o.Slt(exponent, K(255)), result, o.Or(sign, K(0x7f800000u)));
		result = o.Sel(ua.zero, sign, result);
		result = o.Sel(ua.inf, o.Or(sign, K(0x7f800000u)), result);
		const auto field = o.And(o.Shr(a, K(52)), K(0x7ff));
		const auto clamp = o.AndB(o.Ult(K(0x47e), field), o.Ult(field, K(0x7ff)));
		result = o.Sel(clamp, o.Or(sign, K(0x7f7fffffu)), result);
		const auto nan = o.Or(sign, o.Or(K(0x7fc00000u), o.Shr(o.And(a, K(FRACTION)), K(29))));
		result = o.Sel(ua.nan, nan, result);
		const auto flushed = o.Eq(o.And(result, K(0x7f800000u)), K(0));
		return o.Trunc(o.Sel(flushed, sign, result));
	}

private:
	static constexpr uint64_t SIGN     = 0x8000000000000000ull;
	static constexpr uint64_t INF      = 0x7ff0000000000000ull;
	static constexpr uint64_t QUIET    = 0x0008000000000000ull;
	static constexpr uint64_t FRACTION = 0x000fffffffffffffull;
	static constexpr uint64_t HIDDEN   = 0x0010000000000000ull;

	struct Unpacked {
		V sign; // the sign bit, in place
		V m;    // significand with bit 52 set, or 0 for a zero
		V e;    // exponent: the value is m * 2^(e - 1075); signed
		B zero;
		B inf;
		B nan;
	};

	struct Wide {
		V hi;
		V lo;
	};

	struct Operand {
		V value;
		B nan;
	};

	enum class RoundTo { Floor, Ceil, Trunc };

	Ops& o;

	V K(uint64_t value) { return o.K(value); }

	V Jam(V value) { return o.Sel(o.Eq(value, K(0)), K(0), K(1)); }

	V NonNegative(V value) { return o.Sel(o.Slt(value, K(0)), K(0), value); }

	V Quiet(V value) { return o.Or(value, K(QUIET)); }

	// The first NaN operand, quieted, replaces the result.
	template <typename... Rest>
	V PropagateNan(V result, Operand first, Rest... rest) {
		if constexpr (sizeof...(rest) != 0) {
			result = PropagateNan(result, rest...);
		}
		return o.Sel(first.nan, Quiet(first.value), result);
	}

	Unpacked Unpack(V x) {
		const auto field = o.And(o.Shr(x, K(52)), K(0x7ff));
		const auto fraction = o.And(x, K(FRACTION));
		const auto denormal = o.Eq(field, K(0));
		const auto special = o.Eq(field, K(0x7ff));
		const auto zero = o.Eq(o.And(x, K(~SIGN)), K(0));
		const auto raw = o.Sel(denormal, fraction, o.Or(fraction, K(HIDDEN)));
		const auto raw_exp = o.Sel(denormal, K(1), field);
		// A denormal is normalized: its leading bit moves up to bit 52.
		const auto shift = o.Sel(zero, K(0), o.Sub(Clz(raw), K(11)));
		return {o.And(x, K(SIGN)),
		        o.Shl(raw, shift),
		        o.Sub(raw_exp, shift),
		        zero,
		        o.AndB(special, o.Eq(fraction, K(0))),
		        o.AndB(special, o.NotB(o.Eq(fraction, K(0))))};
	}

	// Leading zeros; 64 for zero.
	V Clz(V x) {
		const auto zero = o.Eq(x, K(0));
		auto count = K(0);
		for (const uint64_t step: {32u, 16u, 8u, 4u, 2u, 1u}) {
			const auto empty = o.Eq(o.Shr(x, K(64 - step)), K(0));
			x = o.Sel(empty, o.Shl(x, K(step)), x);
			count = o.Sel(empty, o.Add(count, K(step)), count);
		}
		return o.Sel(zero, K(64), count);
	}

	// x >> n, with a 1 in bit 0 when a set bit was shifted out. n is unsigned, of any size.
	V ShrJam(V x, V n) {
		const auto amount = o.And(n, K(63));
		const auto shifted = o.Shr(x, amount);
		const auto lost = o.Shl(x, o.And(o.Sub(K(64), amount), K(63)));
		const auto jam = o.AndB(o.NotB(o.Eq(amount, K(0))), o.NotB(o.Eq(lost, K(0))));
		const auto result = o.Or(shifted, o.Sel(jam, K(1), K(0)));
		return o.Sel(o.Ult(n, K(64)), result, Jam(x));
	}

	V RoundUp(V mantissa, V rest, uint64_t half) {
		const auto above = o.Ult(K(half), rest);
		const auto tie = o.AndB(o.Eq(rest, K(half)), o.Eq(o.And(mantissa, K(1)), K(1)));
		return o.Sel(o.OrB(above, tie), K(1), K(0));
	}

	// The value sig * 2^q, rounded, with the sign bit `sign`. sig must not be zero.
	V RoundPack(V sign, V q, V sig) {
		const auto lz = Clz(sig);
		const auto normalized = o.Shl(sig, o.And(lz, K(63)));
		const auto exponent = o.Sub(o.Add(q, K(1086)), lz);
		const auto subnormal = o.Slt(exponent, K(1));
		const auto shifted = ShrJam(normalized, o.Sel(subnormal, o.Sub(K(1), exponent), K(0)));
		const auto base = o.Sel(subnormal, K(0), o.Sub(exponent, K(1)));
		auto mantissa = o.Shr(shifted, K(11));
		mantissa = o.Add(mantissa, RoundUp(mantissa, o.And(shifted, K(0x7ff)), 0x400));
		// A carry out of the mantissa raises the exponent, up to infinity.
		const auto result = o.Or(sign, o.Add(o.Shl(o.And(base, K(0x7ff)), K(52)), mantissa));
		return o.Sel(o.Slt(exponent, K(2047)), result, o.Or(sign, K(INF)));
	}

	V AddSigned(V a, V b) {
		const auto ua = Unpack(a), ub = Unpack(b);
		const auto swap = o.Ult(o.And(a, K(~SIGN)), o.And(b, K(~SIGN)));
		const auto big_m = o.Sel(swap, ub.m, ua.m);
		const auto big_e = o.Sel(swap, ub.e, ua.e);
		const auto big_sign = o.Sel(swap, ub.sign, ua.sign);
		const auto small_m = o.Sel(swap, ua.m, ub.m);
		const auto small_e = o.Sel(swap, ua.e, ub.e);
		// Nine guard bits below the significand; the smaller term's lost bits jam.
		const auto big = o.Shl(big_m, K(9));
		const auto small = ShrJam(o.Shl(small_m, K(9)), o.Sub(big_e, small_e));
		const auto same = o.Eq(ua.sign, ub.sign);
		const auto sum = o.Sel(same, o.Add(big, small), o.Sub(big, small));
		const auto sum_zero = o.Eq(sum, K(0));
		auto result =
		    RoundPack(big_sign, o.Sub(big_e, K(1075 + 9)), o.Sel(sum_zero, K(1), sum));
		result = o.Sel(sum_zero, o.And(ua.sign, ub.sign), result);
		const auto both_inf = o.AndB(ua.inf, ub.inf);
		result = o.Sel(o.OrB(ua.inf, ub.inf), o.Sel(ua.inf, a, b), result);
		result = o.Sel(o.AndB(both_inf, o.NotB(same)), K(DEFAULT_NAN), result);
		return PropagateNan(result, Operand {a, ua.nan}, Operand {b, ub.nan});
	}

	// The full 128-bit product.
	Wide MulFull(V x, V y) {
		const auto low = K(0xffffffffu);
		const auto x0 = o.And(x, low), x1 = o.Shr(x, K(32));
		const auto y0 = o.And(y, low), y1 = o.Shr(y, K(32));
		const auto p00 = o.Mul(x0, y0), p01 = o.Mul(x0, y1);
		const auto p10 = o.Mul(x1, y0), p11 = o.Mul(x1, y1);
		const auto middle = o.Add(o.Add(o.Shr(p00, K(32)), o.And(p01, low)), o.And(p10, low));
		const auto lo = o.Or(o.And(p00, low), o.Shl(middle, K(32)));
		const auto hi =
		    o.Add(o.Add(p11, o.Shr(p01, K(32))), o.Add(o.Shr(p10, K(32)), o.Shr(middle, K(32))));
		return {hi, lo};
	}

	Wide Select128(B condition, Wide a, Wide b) {
		return {o.Sel(condition, a.hi, b.hi), o.Sel(condition, a.lo, b.lo)};
	}

	Wide Add128(Wide a, Wide b) {
		const auto lo = o.Add(a.lo, b.lo);
		const auto carry = o.Sel(o.Ult(lo, a.lo), K(1), K(0));
		return {o.Add(o.Add(a.hi, b.hi), carry), lo};
	}

	Wide Sub128(Wide a, Wide b) {
		const auto borrow = o.Sel(o.Ult(a.lo, b.lo), K(1), K(0));
		return {o.Sub(o.Sub(a.hi, b.hi), borrow), o.Sub(a.lo, b.lo)};
	}

	B Ult128(Wide a, Wide b) {
		return o.OrB(o.Ult(a.hi, b.hi), o.AndB(o.Eq(a.hi, b.hi), o.Ult(a.lo, b.lo)));
	}

	V Clz128(Wide x) {
		const auto high_empty = o.Eq(x.hi, K(0));
		return o.Sel(high_empty, o.Add(K(64), Clz(x.lo)), Clz(x.hi));
	}

	// n below 128.
	Wide Shl128(Wide x, V n) {
		const auto past = o.NotB(o.Ult(n, K(64)));
		const auto amount = o.And(n, K(63));
		const auto none = o.Eq(amount, K(0));
		const auto carried = o.Sel(none, K(0), o.Shr(x.lo, o.And(o.Sub(K(64), amount), K(63))));
		const Wide near {o.Or(o.Shl(x.hi, amount), carried), o.Shl(x.lo, amount)};
		const Wide far {o.Shl(x.lo, amount), K(0)};
		return Select128(past, far, near);
	}

	// x >> n with the lost bits jammed into bit 0. n is unsigned, of any size.
	Wide ShrJam128(Wide x, V n) {
		const auto past = o.NotB(o.Ult(n, K(64)));
		const Wide first {o.Sel(past, K(0), x.hi), o.Sel(past, x.hi, x.lo)};
		const auto first_lost = o.AndB(past, o.NotB(o.Eq(x.lo, K(0))));
		const auto amount = o.And(n, K(63));
		const auto none = o.Eq(amount, K(0));
		const auto back = o.And(o.Sub(K(64), amount), K(63));
		const auto lo = o.Or(o.Shr(first.lo, amount), o.Sel(none, K(0), o.Shl(first.hi, back)));
		const auto lost = o.AndB(o.NotB(none), o.NotB(o.Eq(o.Shl(first.lo, back), K(0))));
		const auto jam = o.Sel(o.OrB(first_lost, lost), K(1), K(0));
		const Wide shifted {o.Shr(first.hi, amount), o.Or(lo, jam)};
		const Wide gone {K(0), Jam(o.Or(x.hi, x.lo))};
		return Select128(o.Ult(n, K(128)), shifted, gone);
	}

	B Ordered(V a, V b) { return o.NotB(o.OrB(IsNan(a), IsNan(b))); }

	B IsNan(V x) { return o.Ult(K(INF), o.And(x, K(~SIGN))); }

	// A signed integer in the order of the values; both zeros give 0.
	V Key(V x) {
		const auto magnitude = o.And(x, K(~SIGN));
		return o.Sel(o.Eq(o.And(x, K(SIGN)), K(0)), magnitude, o.Sub(K(0), magnitude));
	}

	V MinMax(V a, V b, bool max_value) {
		const auto ordered = Ordered(a, b);
		const auto first = max_value ? o.Slt(Key(b), Key(a)) : o.Slt(Key(a), Key(b));
		auto result = o.Sel(o.AndB(ordered, first), a, b);
		const auto equal = o.AndB(ordered, o.Eq(Key(a), Key(b)));
		result = o.Sel(equal, max_value ? o.And(a, b) : o.Or(a, b), result);
		result = o.Sel(IsNan(b), a, result);
		return o.Sel(IsNan(a), b, result);
	}

	V Round(V a, RoundTo mode) {
		const auto field = o.And(o.Shr(a, K(52)), K(0x7ff));
		const auto sign = o.And(a, K(SIGN));
		const auto negative = o.NotB(o.Eq(sign, K(0)));
		const auto zero = o.Eq(o.And(a, K(~SIGN)), K(0));
		// From 2^52 up (also infinity and NaN) the value has no fraction.
		const auto integral = o.NotB(o.Ult(field, K(1075)));
		const auto below_one = o.Ult(field, K(1023));
		const auto bits = o.And(o.Sub(K(1075), field), K(63));
		const auto unit = o.Shl(K(1), bits);
		const auto mask = o.Sub(unit, K(1));
		const auto truncated = o.And(a, o.Xor(mask, K(~uint64_t {0})));
		const auto fraction = o.NotB(o.Eq(o.And(a, mask), K(0)));
		V result = truncated;
		V small = sign;
		if (mode == RoundTo::Floor) {
			result = o.Sel(o.AndB(negative, fraction), o.Add(truncated, unit), truncated);
			small = o.Sel(o.AndB(negative, o.NotB(zero)), K(0xbff0000000000000ull), sign);
		} else if (mode == RoundTo::Ceil) {
			result =
			    o.Sel(o.AndB(o.NotB(negative), fraction), o.Add(truncated, unit), truncated);
			small = o.Sel(o.AndB(o.NotB(negative), o.NotB(zero)), K(0x3ff0000000000000ull), sign);
		}
		result = o.Sel(below_one, small, result);
		result = o.Sel(integral, a, result);
		return o.Sel(IsNan(a), Quiet(a), result);
	}
};

} // namespace Libs::Graphics::ShaderRecompiler::SoftFloat64

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_BACKEND_SPIRV_SOFTFLOAT64_H_
