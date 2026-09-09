#include "graphics/shader/recompiler/ir/passes/F64Certificate.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {
using O = ValueOpcode;

// Nonzero values have |x| < 2^upper and are integral multiples of 2^quantum.
// All represented values are finite, and every nonzero one is binary64 normal.
// Zero has no magnitude/grid bounds. integer32 is an additional reciprocal
// input certificate: integral, |x| < 2^32 (signedness does not matter).
struct Facts {
	bool zero = false;
	bool nonzero = false;
	bool integer32 = false;
	int upper = 0;
	int quantum = 0;
};
using Result = std::optional<Facts>;

Result Join(Result a, Result b) {
	if (!a || !b) { return {}; }
	if (a->zero && b->zero) { return Facts{.zero = true, .integer32 = true}; }
	if (a->zero) { b->nonzero = false; return b; }
	if (b->zero) { a->nonzero = false; return a; }
	return Facts{.nonzero = a->nonzero && b->nonzero,
	             .integer32 = a->integer32 && b->integer32,
	             .upper = std::max(a->upper, b->upper),
	             .quantum = std::min(a->quantum, b->quantum)};
}

Result Constant(uint64_t bits) {
	const auto exponent = static_cast<int>((bits >> 52u) & 0x7ffu);
	const uint64_t fraction = bits & 0x000fffffffffffffull;
	if (exponent == 0) {
		if (fraction != 0) { return {}; }
		return Facts{.zero = true, .integer32 = true};
	}
	if (exponent == 0x7ff) { return {}; }
	const uint64_t significand = fraction | (uint64_t{1} << 52u);
	const int upper = exponent - 1023 + 1;
	const int quantum = exponent - 1023 - 52 + std::countr_zero(significand);
	return Facts{.nonzero = true, .integer32 = upper <= 32 && quantum >= 0,
	             .upper = upper, .quantum = quantum};
}

struct Bits {
	uint32_t zero = 0;
	uint32_t one = 0;
	bool Complete() const { return (zero | one) == UINT32_MAX; }
};
Bits Literal(uint32_t value) { return {~value, value}; }
Bits Intersect(Bits a, Bits b) { return {a.zero & b.zero, a.one & b.one}; }

// Exact bitwise abstraction of a+b(+carry), including guest U32 wrap. It does
// not use host signed arithmetic or assume that carries are independent bits.
Bits Add(Bits a, Bits b, unsigned carry = 1u) {
	Bits result;
	for (unsigned bit = 0; bit < 32; ++bit) {
		const unsigned av = (a.zero >> bit) & 1u ? 1u : (a.one >> bit) & 1u ? 2u : 3u;
		const unsigned bv = (b.zero >> bit) & 1u ? 1u : (b.one >> bit) & 1u ? 2u : 3u;
		unsigned outputs = 0, next = 0;
		for (unsigned x = 0; x < 2; ++x) for (unsigned y = 0; y < 2; ++y)
			for (unsigned c = 0; c < 2; ++c) {
				if ((av & (1u << x)) && (bv & (1u << y)) && (carry & (1u << c))) {
					outputs |= 1u << ((x + y + c) & 1u);
					next |= 1u << ((x + y + c) >> 1u);
				}
			}
		if (outputs == 1u) { result.zero |= 1u << bit; }
		if (outputs == 2u) { result.one |= 1u << bit; }
		carry = next;
	}
	return result;
}

Bits Shift(Bits value, unsigned amount, O op) {
	if (amount == 0) { return value; }
	if (op == O::ShiftLeftLogical32) {
		return {(value.zero << amount) | ((1u << amount) - 1u), value.one << amount};
	}
	const uint32_t fill = UINT32_MAX << (32u - amount);
	Bits result{value.zero >> amount, value.one >> amount};
	if (op == O::ShiftRightLogical32 || (value.zero & 0x80000000u)) { result.zero |= fill; }
	if (op == O::ShiftRightArithmetic32 && (value.one & 0x80000000u)) { result.one |= fill; }
	return result;
}

// Identity chains and recursive Phi proofs are bounded. Unproved cycles remain
// unknown; a size/depth budget is a conservative rejection, never an assumption.
class Analyzer {
public:
	Result F64(Value value) {
		value = Resolve(value);
		if (value.IsImmediate() && value.GetType() == Type::F64) { return Constant(value.F64Bits()); }
		auto* inst = value.TryInstruction();
		if (inst == nullptr || depth >= 512) { return {}; }
		if (const auto found = facts.find(inst); found != facts.end()) { return found->second; }
		if (!active_facts.insert(inst).second) { return {}; }
		++depth;
		const auto result = Evaluate(*inst);
		--depth;
		active_facts.erase(inst);
		facts.emplace(inst, result);
		return result;
	}

	Result Pair(Value low, Value high) {
		low = ResolveSelectedWord(low); high = ResolveSelectedWord(high);
		if (depth >= 512) { return {}; }
		const PairKey key{Key(low), Key(high)};
		if (const auto found = pairs.find(key); found != pairs.end()) { return found->second; }
		if (!active_pairs.insert(key).second) { return {}; }
		++depth;
		const auto result = EvaluatePair(low, high);
		--depth;
		active_pairs.erase(key);
		pairs.emplace(key, result);
		return result;
	}

private:
	struct ValueKey {
		const Inst* inst = nullptr;
		uint64_t bits = 0;
		Type type = Type::Void;
		bool operator==(const ValueKey&) const = default;
	};
	struct PairKey {
		ValueKey low, high;
		bool operator==(const PairKey&) const = default;
	};
	struct PairHash {
		size_t operator()(const PairKey& key) const {
			const auto part = [](const ValueKey& value) {
				return std::hash<const Inst*>{}(value.inst) ^ std::hash<uint64_t>{}(value.bits) ^
				       (static_cast<size_t>(value.type) << 1u);
			};
			return part(key.low) ^ (part(key.high) << 1u);
		}
	};
	static ValueKey Key(Value value) {
		if (auto* inst = value.TryInstruction()) { return {.inst = inst}; }
		return {.bits = value.IsImmediate() && value.GetType() == Type::U32 ? value.U32() : 0,
		        .type = value.GetType()};
	}
	static Value Resolve(Value value) {
		for (unsigned count = 0; count < 512; ++count) {
			auto* inst = value.TryInstruction();
			if (inst == nullptr || inst->GetOpcode() != O::Identity) { return value; }
			value = inst->Arg(0);
		}
		return {};
	}
	Value ResolveSelectedWord(Value value) {
		// Constant propagation can fold only one word of a predicated pair.
		// Selecting a provably taken arm independently is exact; unknown
		// predicates still require the paired, correlated join below.
		for (unsigned count = 0; count < 512; ++count) {
			value = Resolve(value);
			auto* inst = value.TryInstruction();
			if (inst == nullptr || inst->GetOpcode() != O::SelectU32) { return value; }
			const auto condition = Boolean(inst->Arg(0));
			if (!condition) { return value; }
			value = inst->Arg(*condition ? 1 : 2);
		}
		return {};
	}
	static bool Immediate32(Value value, uint32_t expected) {
		value = Resolve(value);
		return value.IsImmediate() && value.GetType() == Type::U32 && value.U32() == expected;
	}

	std::optional<bool> Boolean(Value value) {
		value = Resolve(value);
		if (value.IsImmediate() && value.GetType() == Type::U1) { return value.U1(); }
		auto* inst = value.TryInstruction();
		if (inst == nullptr || depth >= 512) { return {}; }
		if (const auto found = booleans.find(inst); found != booleans.end()) { return found->second; }
		if (!active_bool.insert(inst).second) { return {}; }
		++depth;
		std::optional<bool> result;
		switch (inst->GetOpcode()) {
			case O::IEqual32: case O::INotEqual32: {
				const auto a = KnownBits(inst->Arg(0)), b = KnownBits(inst->Arg(1));
				if ((a.one & b.zero) || (a.zero & b.one)) { result = false; }
				else if (a.Complete() && b.Complete()) { result = a.one == b.one; }
				if (result && inst->GetOpcode() == O::INotEqual32) { result = !*result; }
				break;
			}
			case O::LogicalNot: {
				if (auto a = Boolean(inst->Arg(0))) { result = !*a; }
				break;
			}
			case O::LogicalAnd: case O::LogicalOr: case O::LogicalXor: {
				const auto a = Boolean(inst->Arg(0)), b = Boolean(inst->Arg(1));
				if (a && b) {
					result = inst->GetOpcode() == O::LogicalAnd ? *a && *b :
					         inst->GetOpcode() == O::LogicalOr ? *a || *b : *a != *b;
				} else if (inst->GetOpcode() == O::LogicalAnd && ((a && !*a) || (b && !*b))) {
					result = false;
				} else if (inst->GetOpcode() == O::LogicalOr && ((a && *a) || (b && *b))) {
					result = true;
				}
				break;
			}
			case O::SelectU1: {
				if (auto condition = Boolean(inst->Arg(0))) { result = Boolean(inst->Arg(*condition ? 1 : 2)); }
				else { const auto a = Boolean(inst->Arg(1)), b = Boolean(inst->Arg(2)); if (a == b) { result = a; } }
				break;
			}
			default: break;
		}
		--depth; active_bool.erase(inst); booleans.emplace(inst, result);
		return result;
	}

	Bits KnownBits(Value value) {
		value = Resolve(value);
		if (value.IsImmediate() && value.GetType() == Type::U32) { return Literal(value.U32()); }
		auto* inst = value.TryInstruction();
		if (inst == nullptr || depth >= 512) { return {}; }
		if (const auto found = bits.find(inst); found != bits.end()) { return found->second; }
		if (!active_bits.insert(inst).second) { return {}; }
		++depth;
		const auto result = EvaluateBits(*inst);
		--depth; active_bits.erase(inst); bits.emplace(inst, result);
		return result;
	}

	Bits EvaluateBits(const Inst& inst) {
		const auto op = inst.GetOpcode();
		if (op == O::SelectU32) {
			if (auto condition = Boolean(inst.Arg(0))) { return KnownBits(inst.Arg(*condition ? 1 : 2)); }
			return Intersect(KnownBits(inst.Arg(1)), KnownBits(inst.Arg(2)));
		}
		if (op == O::Phi && inst.NumArgs() != 0) {
			auto result = KnownBits(inst.Arg(0));
			for (size_t i = 1; i < inst.NumArgs(); ++i) { result = Intersect(result, KnownBits(inst.Arg(i))); }
			return result;
		}
		if (inst.NumArgs() == 0) { return {}; }
		const auto a = KnownBits(inst.Arg(0));
		if (op == O::BitwiseNot32) { return {a.one, a.zero}; }
		if (inst.NumArgs() < 2) { return {}; }
		const auto b = KnownBits(inst.Arg(1));
		switch (op) {
			case O::BitwiseAnd32: return {a.zero | b.zero, a.one & b.one};
			case O::BitwiseOr32: return {a.zero & b.zero, a.one | b.one};
			case O::BitwiseXor32: return {(a.zero & b.zero) | (a.one & b.one), (a.one & b.zero) | (a.zero & b.one)};
			case O::IAdd32: return Add(a, b);
			case O::ISub32: return Add(a, {b.one, b.zero}, 2u);
			case O::IMul32: {
				if (a.Complete() && b.Complete()) { return Literal(a.one * b.one); }
				if (a.Complete() && std::has_single_bit(a.one)) { return Shift(b, std::countr_zero(a.one), O::ShiftLeftLogical32); }
				if (b.Complete() && std::has_single_bit(b.one)) { return Shift(a, std::countr_zero(b.one), O::ShiftLeftLogical32); }
				const auto zeros = std::min(32, std::countr_one(a.zero) + std::countr_one(b.zero));
				return {zeros == 32 ? UINT32_MAX : (1u << zeros) - 1u, a.one & b.one & 1u};
			}
			case O::ShiftLeftLogical32: case O::ShiftRightLogical32: case O::ShiftRightArithmetic32: {
				if ((b.zero & 0xffffffe0u) != 0xffffffe0u) { return {}; }
				Bits result{UINT32_MAX, UINT32_MAX};
				for (unsigned amount = 0; amount < 32; ++amount) {
					if ((amount & b.zero) == 0 && (amount & b.one) == b.one) { result = Intersect(result, Shift(a, amount, op)); }
				}
				return result;
			}
			case O::BitFieldUExtract: case O::BitFieldSExtract: {
				const auto width = KnownBits(inst.Arg(2));
				if (!b.Complete() || !width.Complete() || b.one >= 32 || width.one == 0 || width.one > 32 - b.one) { return {}; }
				const uint32_t mask = width.one == 32 ? UINT32_MAX : (1u << width.one) - 1u;
				Bits result{(a.zero >> b.one) & mask, (a.one >> b.one) & mask};
				if (op == O::BitFieldUExtract || (result.zero & (1u << (width.one - 1u)))) { result.zero |= ~mask; }
				if (op == O::BitFieldSExtract && (result.one & (1u << (width.one - 1u)))) { result.one |= ~mask; }
				return result;
			}
			default: return {};
		}
	}

	Result EvaluatePair(Value low, Value high) {
		if (low.IsImmediate() && low.GetType() == Type::U32 && high.IsImmediate() && high.GetType() == Type::U32) {
			return Constant(uint64_t{low.U32()} | (uint64_t{high.U32()} << 32u));
		}
		auto* lo = low.TryInstruction(); auto* hi = high.TryInstruction();
		if (lo == nullptr || hi == nullptr) { return {}; }
		if (lo->GetOpcode() == O::CompositeExtractF64 && hi->GetOpcode() == O::CompositeExtractF64 &&
		    Immediate32(lo->Arg(1), 0) && Immediate32(hi->Arg(1), 1) && Resolve(lo->Arg(0)) == Resolve(hi->Arg(0))) {
			return F64(lo->Arg(0));
		}
		if (lo->GetOpcode() == O::SelectU32 && hi->GetOpcode() == O::SelectU32 && Resolve(lo->Arg(0)) == Resolve(hi->Arg(0))) {
			if (auto condition = Boolean(lo->Arg(0))) { return Pair(lo->Arg(*condition ? 1 : 2), hi->Arg(*condition ? 1 : 2)); }
			return Join(Pair(lo->Arg(1), hi->Arg(1)), Pair(lo->Arg(2), hi->Arg(2)));
		}
		if (lo->GetOpcode() == O::Phi && hi->GetOpcode() == O::Phi && lo->Parent() == hi->Parent() &&
		    lo->NumArgs() != 0 && lo->NumArgs() == hi->NumArgs() && lo->NumArgs() == lo->NumPhiBlocks() && hi->NumArgs() == hi->NumPhiBlocks()) {
			Result result;
			std::unordered_set<const Block*> seen;
			for (size_t i = 0; i < lo->NumArgs(); ++i) {
				const auto* predecessor = lo->PhiBlock(i);
				if (predecessor == nullptr || !seen.insert(predecessor).second) { return {}; }
				size_t match = hi->NumArgs();
				for (size_t j = 0; j < hi->NumArgs(); ++j) {
					if (hi->PhiBlock(j) == predecessor) { if (match != hi->NumArgs()) { return {}; } match = j; }
				}
				if (match == hi->NumArgs()) { return {}; }
				auto edge = Pair(lo->Arg(i), hi->Arg(match));
				if (!edge) { return {}; }
				result = i == 0 ? edge : Join(result, edge);
			}
			return result;
		}
		return {};
	}

	Result Evaluate(const Inst& inst) {
		switch (inst.GetOpcode()) {
			case O::CompositeConstructF64: return Pair(inst.Arg(0), inst.Arg(1));
			case O::ConvertF64S32: case O::ConvertF64U32: {
				const auto source = KnownBits(inst.Arg(0));
				if (source.Complete() && source.one == 0) { return Facts{.zero = true, .integer32 = true}; }
				return Facts{.nonzero = source.one != 0, .integer32 = true, .upper = 32, .quantum = 0};
			}
			case O::FPAbs64: case O::FPNeg64: return F64(inst.Arg(0));
			case O::FPRecip64: {
				const auto a = F64(inst.Arg(0));
				if (!a || !a->integer32 || !a->nonzero) { return {}; }
				// FDiv seed followed by two correctly rounded FmaKHR instructions.
				// For 1 <= |d| < 2^32 all intermediates are zero/normal (minimum
				// exact grid 2^-170). Final 2^-33 < |r| < 2, error < 2^12 ULP64.
				return Facts{.nonzero = true, .upper = 1, .quantum = -85};
			}
			case O::FPMul64: case O::FPFma64: {
				const auto a = F64(inst.Arg(0)), b = F64(inst.Arg(1));
				if (!a || !b) { return {}; }
				const bool product_zero = a->zero || b->zero;
				int upper = product_zero ? 0 : a->upper + b->upper;
				int quantum = product_zero ? 0 : a->quantum + b->quantum;
				if (inst.GetOpcode() == O::FPFma64) {
					const auto c = F64(inst.Arg(2));
					if (!c) { return {}; }
					if (product_zero) { return c; }
					if (!c->zero) { upper = std::max(upper, c->upper) + 1; quantum = std::min(quantum, c->quantum); }
				} else if (product_zero) { return Facts{.zero = true, .integer32 = true}; }
				++upper; // include rounding across a power-of-two boundary
				if (quantum < -1022 || upper > 1023) { return {}; }
				return Facts{.upper = upper, .quantum = quantum};
			}
			default: return {};
		}
	}

	unsigned depth = 0;
	std::unordered_map<const Inst*, Result> facts;
	std::unordered_map<const Inst*, Bits> bits;
	std::unordered_map<const Inst*, std::optional<bool>> booleans;
	std::unordered_set<const Inst*> active_facts, active_bits, active_bool;
	std::unordered_map<PairKey, Result, PairHash> pairs;
	std::unordered_set<PairKey, PairHash> active_pairs;
};
} // namespace

F64Certificate AnalyzeF64Program(const Program& program,
                                const ShaderFloatingPointState& initial_fp_state,
                                const ShaderHostProfile& host_profile) {
	F64Certificate result;
	// Certify every instruction the emitter will retain. Normal translation
	// eliminates dead guest arithmetic before this point, but callers may also
	// emit already planned IR directly without running another DCE pass.
	for (const auto* block: program.blocks) for (const auto& inst: *block) {
		switch (inst.GetOpcode()) {
			case O::FPRecip64: case O::FPFma64: case O::FPMul64:
				// MUL uses FmaKHR(a,b,-0), avoiding weaker portable DP multiply precision.
				result.needs_fma64 = true; result.needs_native64 = true; break;
			case O::ConvertF32F64: result.needs_native64 = true; result.needs_narrow_f32 = true; break;
			default: break;
		}
	}
	if (!result.needs_native64) { return result; }
	const auto fail = [&](const char* message, const Inst* inst = nullptr) {
		result.failed_inst = inst;
		result.error = std::string("FP64 certificate: ") + message;
		return result;
	};
	if (!initial_fp_state.known) { return fail("initial guest FP state is unknown"); }
	if (!program.fp_mode_inspected) { return fail("guest FP mode was not inspected"); }
	if (program.writes_fp_mode) { return fail("guest MODE writes are unsupported"); }
	if (((initial_fp_state.float_mode >> 2u) & 3u) != 0) { return fail("guest DP rounding mode is unsupported"); }
	if (result.needs_narrow_f32 && (initial_fp_state.float_mode & 3u) != 0) { return fail("guest SP rounding mode is unsupported"); }
	if (!host_profile.known) { return fail("host profile is unknown"); }
	if (!host_profile.float64) { return fail("host float64 is unavailable"); }
	if (!host_profile.rte_float64) { return fail("host rte_float64 is unavailable"); }
	if (!host_profile.signed_zero_inf_nan_preserve_float64) { return fail("host signed_zero_inf_nan_preserve_float64 is unavailable"); }
	if (result.needs_fma64 && !host_profile.fma_float64) { return fail("host fma_float64 is unavailable"); }
	if (result.needs_narrow_f32 && !host_profile.rte_float32) { return fail("host rte_float32 is unavailable"); }
	Analyzer analyzer;
	for (const auto* block: program.blocks) for (const auto& inst: *block) {
		if (inst.GetOpcode() == O::FPMul64 || inst.GetOpcode() == O::FPFma64 || inst.GetOpcode() == O::FPRecip64) {
			if (!analyzer.F64(Value(const_cast<Inst*>(&inst)))) { return fail("unproven finite normal FP64 dataflow", &inst); }
		} else if (inst.GetOpcode() == O::ConvertF32F64) {
			const auto source = analyzer.F64(inst.Arg(0));
			if (!source) { return fail("unproven finite normal FP64 dataflow", &inst); }
			if (!source->zero && (source->quantum < -126 || source->upper > 127)) { return fail("FP32 narrowing range is unproven", &inst); }
		}
	}
	return result;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
