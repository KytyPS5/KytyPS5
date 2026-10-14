#include "graphics/shader/recompiler/backend/spirv/softFloat64.h"

#include <array>
#include <bit>
#include <cfloat>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

// The software 64-bit floats against the CPU's own doubles, which round to nearest even and keep
// denormals. Arithmetic NaNs are compared as NaN only: the payload is not part of the contract.

namespace {

using Libs::Graphics::ShaderRecompiler::SoftFloat64::Math;

struct CpuOps {
	using V = uint64_t;
	using W = uint32_t;
	using B = bool;

	static V K(uint64_t value) { return value; }
	static V Add(V a, V b) { return a + b; }
	static V Sub(V a, V b) { return a - b; }
	static V Mul(V a, V b) { return a * b; }
	static V And(V a, V b) { return a & b; }
	static V Or(V a, V b) { return a | b; }
	static V Xor(V a, V b) { return a ^ b; }
	static V Shl(V a, V n) { return a << Amount(n); }
	static V Shr(V a, V n) { return a >> Amount(n); }
	static B Eq(V a, V b) { return a == b; }
	static B Ult(V a, V b) { return a < b; }
	static B Slt(V a, V b) { return static_cast<int64_t>(a) < static_cast<int64_t>(b); }
	static B AndB(B a, B b) { return a && b; }
	static B OrB(B a, B b) { return a || b; }
	static B NotB(B a) { return !a; }
	static V Sel(B condition, V a, V b) { return condition ? a : b; }
	static V Zext(W value) { return value; }
	static W Trunc(V value) { return static_cast<W>(value); }

	// A shift by 64 or more is undefined in SPIR-V too: the algorithm must never make one.
	static V Amount(V n) {
		if (n >= 64u) {
			std::printf("shift amount %" PRIu64 "\n", n);
			std::abort();
		}
		return n;
	}
};

CpuOps          g_ops;
Math<CpuOps>    g_math(g_ops);
uint64_t        g_checks   = 0;
uint64_t        g_failures = 0;

uint64_t Bits(double value) {
	return std::bit_cast<uint64_t>(value);
}

double Double(uint64_t bits) {
	return std::bit_cast<double>(bits);
}

bool IsNan(uint64_t bits) {
	return (bits & 0x7fffffffffffffffull) > 0x7ff0000000000000ull;
}

void Expect(const char* op, std::initializer_list<uint64_t> inputs, uint64_t expected,
            uint64_t actual, bool nan_any) {
	g_checks++;
	if (nan_any && IsNan(expected) && IsNan(actual)) {
		return;
	}
	if (expected == actual) {
		return;
	}
	if (g_failures++ < 20) {
		std::printf("%s(", op);
		for (const auto input: inputs) {
			std::printf(" %016" PRIx64, input);
		}
		std::printf(" ): expected %016" PRIx64 ", got %016" PRIx64 "\n", expected, actual);
	}
}

// Floats as the native emitter handles them: a denormal float is flushed to a zero of its sign.
float FlushF32(float value) {
	return std::fpclassify(value) == FP_SUBNORMAL ? std::copysign(0.0f, value) : value;
}

uint32_t ReferenceToF32(uint64_t bits) {
	const auto field = (bits >> 52u) & 0x7ffu;
	if (field > 0x47eu && field < 0x7ffu) {
		return (static_cast<uint32_t>(bits >> 32u) & 0x80000000u) | 0x7f7fffffu;
	}
	return std::bit_cast<uint32_t>(FlushF32(static_cast<float>(Double(bits))));
}

uint64_t ReferenceMinMax(uint64_t a, uint64_t b, bool max_value) {
	if (IsNan(a)) {
		return b;
	}
	if (IsNan(b)) {
		return a;
	}
	if (Double(a) == Double(b)) {
		return max_value ? (a & b) : (a | b);
	}
	return (max_value ? Double(a) > Double(b) : Double(a) < Double(b)) ? a : b;
}

void CheckValue(uint64_t a) {
	const auto x = Double(a);
	Expect("recip", {a}, Bits(1.0 / x), g_math.Recip(a), true);
	Expect("floor", {a}, Bits(std::floor(x)), g_math.Floor(a), true);
	Expect("ceil", {a}, Bits(std::ceil(x)), g_math.Ceil(a), true);
	Expect("trunc", {a}, Bits(std::trunc(x)), g_math.Trunc(a), true);
	Expect("fract", {a}, Bits(x - std::floor(x)), g_math.Fract(a), true);
	const auto narrowed = g_math.ToF32(a);
	const auto reference = ReferenceToF32(a);
	const bool both_nan = std::isnan(std::bit_cast<float>(narrowed)) &&
	                      std::isnan(std::bit_cast<float>(reference));
	Expect("to_f32", {a}, both_nan ? 0 : reference, both_nan ? 0 : narrowed, false);
}

void CheckPair(uint64_t a, uint64_t b) {
	const auto x = Double(a), y = Double(b);
	Expect("add", {a, b}, Bits(x + y), g_math.Add(a, b), true);
	Expect("sub", {a, b}, Bits(x - y), g_math.Sub(a, b), true);
	Expect("mul", {a, b}, Bits(x * y), g_math.Mul(a, b), true);
	Expect("min", {a, b}, ReferenceMinMax(a, b, false), g_math.Min(a, b), false);
	Expect("max", {a, b}, ReferenceMinMax(a, b, true), g_math.Max(a, b), false);
	Expect("eq", {a, b}, x == y, g_math.OrdEqual(a, b), false);
	Expect("le", {a, b}, x <= y, g_math.OrdLessThanEqual(a, b), false);
	Expect("ge", {a, b}, x >= y, g_math.OrdGreaterThanEqual(a, b), false);
}

void CheckTriple(uint64_t a, uint64_t b, uint64_t c) {
	Expect("fma", {a, b, c}, Bits(std::fma(Double(a), Double(b), Double(c))),
	       g_math.Fma(a, b, c), true);
}

void CheckF32(uint32_t bits) {
	const auto widened = Bits(static_cast<double>(FlushF32(std::bit_cast<float>(bits))));
	Expect("from_f32", {bits}, widened, g_math.FromF32(bits), true);
	Expect("from_s32", {bits}, Bits(static_cast<double>(static_cast<int32_t>(bits))),
	       g_math.FromS32(bits), false);
	Expect("from_u32", {bits}, Bits(static_cast<double>(bits)), g_math.FromU32(bits), false);
}

const std::vector<uint64_t>& Edges() {
	static const std::vector<uint64_t> edges = [] {
		std::vector<uint64_t> list {
		    0,
		    1,                       // smallest denormal
		    2,
		    0x000fffffffffffffull, // largest denormal
		    0x0010000000000000ull, // smallest normal
		    0x0010000000000001ull,
		    0x3fe0000000000000ull, // 0.5
		    0x3fefffffffffffffull,
		    0x3ff0000000000000ull, // 1
		    0x3ff0000000000001ull,
		    0x3ff8000000000000ull, // 1.5
		    0x4000000000000000ull, // 2
		    0x4004000000000000ull, // 2.5
		    0x400a000000000000ull, // 3.25
		    0x4330000000000000ull, // 2^52
		    0x4330000000000001ull,
		    0x4340000000000000ull, // 2^53
		    0x4340000000000001ull,
		    0x47efffffe0000000ull, // float max
		    0x47efffffefffffffull, // just below the float rounding point
		    0x47effffff0000000ull, // float rounding tie to 2^128
		    0x47f0000000000000ull, // 2^128
		    0x36a0000000000000ull, // smallest float denormal
		    0x3810000000000000ull, // smallest float normal
		    0x380fffffffffffffull,
		    0x7fefffffffffffffull, // largest
		    0x7fe0000000000000ull,
		    0x7ff0000000000000ull, // infinity
		    0x7ff8000000000000ull, // quiet NaN
		    0x7ff8123456789abcull,
		    0x7ff0000000000001ull, // signaling NaN
		    0x3cb0000000000000ull, // 2^-52
		    0x3ca0000000000000ull, // 2^-53
		    0x3c90000000000000ull, // 2^-54
		};
		const auto count = list.size();
		for (size_t i = 0; i < count; i++) {
			list.push_back(list[i] | 0x8000000000000000ull);
		}
		return list;
	}();
	return edges;
}

class Inputs {
public:
	explicit Inputs(uint64_t seed): m_random(seed) {}

	// Random bits, values near the edges of the exponent range, and edge values.
	uint64_t Next() {
		switch (m_random() % 6u) {
			case 0: return m_random();
			case 1: return WithExponent(m_random() % 0x7ffu);
			case 2: return WithExponent(1023u - 64u + m_random() % 128u);
			case 3: return WithExponent(m_random() % 64u);
			case 4: return WithExponent(2046u - m_random() % 64u);
			default: {
				const auto& edges = Edges();
				return edges[m_random() % edges.size()];
			}
		}
	}

	// A value close to `a`: the same exponent or one near it, often its negation, so that
	// sums cancel and round at a tie.
	uint64_t Near(uint64_t a) {
		auto b = a;
		switch (m_random() % 4u) {
			case 0: b ^= m_random() & 0xffu; break;
			case 1: b += (m_random() % 128u) << 52u; break;
			case 2: b -= (m_random() % 128u) << 52u; break;
			default: b = (a & 0xfff0000000000000ull) | (m_random() & 0x000fffffffffffffull); break;
		}
		if ((m_random() & 1u) != 0) {
			b ^= 0x8000000000000000ull;
		}
		return b;
	}

	uint64_t Raw() { return m_random(); }

private:
	uint64_t WithExponent(uint64_t field) {
		return (m_random() & 0x800fffffffffffffull) | (field << 52u);
	}

	std::mt19937_64 m_random;
};

void Run(const char* name, uint64_t count, void (*body)(Inputs&, uint64_t)) {
	Inputs inputs(0x5eed0000u + count);
	const auto failures = g_failures;
	body(inputs, count);
	std::printf("%-8s %s\n", name, g_failures == failures ? "ok" : "FAILED");
}

} // namespace

int main() {
	Run("edges", 0, [](Inputs&, uint64_t) {
		for (const auto a: Edges()) {
			CheckValue(a);
			for (const auto b: Edges()) {
				CheckPair(a, b);
				for (const auto c: {0ull, 0x8000000000000000ull, 0x3ff0000000000000ull,
				                    0xbcb0000000000000ull, 0x7ff0000000000000ull}) {
					CheckTriple(a, b, c);
				}
			}
		}
	});
	Run("unary", 1000000, [](Inputs& inputs, uint64_t count) {
		for (uint64_t i = 0; i < count; i++) {
			CheckValue(inputs.Next());
		}
	});
	Run("binary", 1000000, [](Inputs& inputs, uint64_t count) {
		for (uint64_t i = 0; i < count; i++) {
			const auto a = inputs.Next();
			CheckPair(a, (i & 1u) != 0 ? inputs.Near(a) : inputs.Next());
		}
	});
	Run("fma", 1000000, [](Inputs& inputs, uint64_t count) {
		for (uint64_t i = 0; i < count; i++) {
			const auto a = inputs.Next(), b = inputs.Next();
			// Half of the cases cancel: c is close to the negated product.
			const auto product = Bits(Double(a) * Double(b));
			const auto c = (i & 1u) != 0 ? inputs.Near(product ^ 0x8000000000000000ull)
			                             : inputs.Next();
			CheckTriple(a, b, c);
		}
	});
	Run("f32", 1000000, [](Inputs& inputs, uint64_t count) {
		for (uint64_t i = 0; i < count; i++) {
			CheckF32(static_cast<uint32_t>(inputs.Raw()));
		}
		for (const uint32_t bits: {0u, 0x80000000u, 1u, 0x807fffffu, 0x00800000u, 0x7f800000u,
		                           0xff800000u, 0x7fc00000u, 0x7f800001u, 0x7fffffffu,
		                           0xffffffffu, 0x7f7fffffu}) {
			CheckF32(bits);
		}
	});
	std::printf("%" PRIu64 " checks, %" PRIu64 " failures\n", g_checks, g_failures);
	if (g_failures != 0) {
		std::printf("soft float64 tests failed\n");
		return EXIT_FAILURE;
	}
	std::printf("soft float64 tests passed\n");
	return EXIT_SUCCESS;
}
