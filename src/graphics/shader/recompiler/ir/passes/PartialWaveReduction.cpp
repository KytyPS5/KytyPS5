#include "graphics/shader/recompiler/ir/passes/PartialWaveReduction.h"

#include <cstdlib>
#include <string>
#include <optional>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

bool PartialWaveReductionEnabled() {
	static const bool enabled = [] {
		const char* v = std::getenv("KYTY_PARTIAL_WAVE_REDUCTION");
		return v != nullptr && v[0] == '1';
	}();
	return enabled;
}

namespace {

struct Reduction {
	WaveReduceOp op;
	Value        input; // value fed to the first DPP step (neutral already applied)
};

// Rejection diagnostic (KYTY_DBG_LANE_AUDIT): the deepest matcher stage that failed wins.
struct RejectDiag {
	int         stage = -1;
	std::string text;
	void        Note(int s, std::string t) {
		if (s >= stage) {
			stage = s;
			text  = std::move(t);
		}
	}
};
thread_local RejectDiag g_diag;

std::string OpName(Value value) {
	value = value.Resolve();
	if (value.IsImmediate()) return "imm";
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) return "none";
	return std::string(ValueOpcodeName(inst->GetOpcode()));
}

Inst* InstOf(Value value) {
	return value.Resolve().TryInstruction();
}

// EXEC proven to be all ones for the existing invocations: Or(Not(x), x) or a literal true.
bool IsAllTrue(Value value) {
	value = value.Resolve();
	if (value.IsImmediate()) return value.GetType() == Type::U1 && value.U1();
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || inst->GetOpcode() != ValueOpcode::LogicalOr) return false;
	const auto a = inst->Arg(0).Resolve();
	const auto b = inst->Arg(1).Resolve();
	const auto is_not_of = [](Value n, Value x) {
		const auto* ni = n.TryInstruction();
		return ni != nullptr && ni->GetOpcode() == ValueOpcode::LogicalNot &&
		       ni->Arg(0).Resolve() == x;
	};
	return is_not_of(a, b) || is_not_of(b, a);
}

// Start of the whole-wave (s_or_saveexec -1) region of the reduction being matched: the
// definition of the all-true EXEC value. Compares defined after it in the same block ran with
// every lane active, so their lane-mask bits are not zero for never-launched lanes.
thread_local const Inst* g_whole_region = nullptr;

// Vector float compares: V_CMP*/V_CMPX* write 0 to the mask bit of every lane that is inactive
// under the EXEC in effect when they execute. (Integer compares are not accepted: the IR does not
// distinguish them from uniform s_cmp results.)
bool IsVectorFloatCompare(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::FPOrdEqual32:
		case ValueOpcode::FPUnordEqual32:
		case ValueOpcode::FPOrdNotEqual32:
		case ValueOpcode::FPUnordNotEqual32:
		case ValueOpcode::FPOrdLessThan32:
		case ValueOpcode::FPUnordLessThan32:
		case ValueOpcode::FPOrdGreaterThan32:
		case ValueOpcode::FPUnordGreaterThan32:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPUnordLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPUnordGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPCmpClass32: return true;
		default: return false;
	}
}

// True when `inst` follows the whole-wave region start in the same block; also true when the
// region start is unknown (nothing can then be proven).
bool DefinedInWholeWaveRegion(const Inst* inst) {
	const auto* start = g_whole_region;
	if (start == nullptr) return true;
	if (inst->Parent() != start->Parent()) return false;
	bool after = false;
	for (auto& i: *start->Parent()) {
		if (&i == start) after = true;
		if (&i == inst) return after;
	}
	return true;
}

// A predicate that can only be true in invocations whose EXEC bit is set, i.e. only in
// invocations that exist. The initial EXEC is the literal true; refinement is And / Phi only.
bool IsExecBounded(Value value, std::unordered_set<const Inst*>& visiting) {
	value = value.Resolve();
	if (value.IsImmediate()) return value.GetType() == Type::U1;
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) return false;
	if (!visiting.insert(inst).second) return true; // cycle: decided by the other incomings
	bool result = false;
	switch (inst->GetOpcode()) {
		case ValueOpcode::LogicalAnd:
			result = IsExecBounded(inst->Arg(0), visiting) || IsExecBounded(inst->Arg(1), visiting);
			break;
		case ValueOpcode::LogicalOr:
			// Or is false in a lane only if both terms are: both must be bounded.
			result = IsExecBounded(inst->Arg(0), visiting) && IsExecBounded(inst->Arg(1), visiting);
			break;
		case ValueOpcode::Phi:
			result = inst->NumArgs() != 0;
			for (size_t i = 0; result && i < inst->NumArgs(); i++) {
				result = IsExecBounded(inst->Arg(i), visiting);
			}
			break;
		default:
			result = IsVectorFloatCompare(inst->GetOpcode()) && !DefinedInWholeWaveRegion(inst);
			break;
	}
	return result;
}

// Select(all-true, a, b) is just a.
Value StripAllTrueSelect(Value value) {
	for (;;) {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::SelectU32 ||
		    !IsAllTrue(inst->Arg(0))) {
			return value;
		}
		value = inst->Arg(1);
	}
}

std::optional<WaveReduceOp> ReduceOpOf(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::UMin32: return WaveReduceOp::UMin;
		case ValueOpcode::SMin32: return WaveReduceOp::SMin;
		case ValueOpcode::UMax32: return WaveReduceOp::UMax;
		case ValueOpcode::SMax32: return WaveReduceOp::SMax;
		case ValueOpcode::BitwiseOr32: return WaveReduceOp::Or;
		case ValueOpcode::BitwiseAnd32: return WaveReduceOp::And;
		default: return std::nullopt;
	}
}

uint32_t NeutralOf(WaveReduceOp op) {
	switch (op) {
		case WaveReduceOp::UMin: return 0xffffffffu;
		case WaveReduceOp::SMin: return 0x7fffffffu;
		case WaveReduceOp::UMax: return 0u;
		case WaveReduceOp::SMax: return 0x80000000u;
		case WaveReduceOp::Or: return 0u;
		case WaveReduceOp::And: return 0xffffffffu;
	}
	return 0;
}

// DppUpdate(Op(DppMove(Y, E), Y), Y, E) with the given row_shr amount; returns Y.
std::optional<Value> MatchRowShrStep(Value value, ValueOpcode opcode, uint32_t shift) {
	const auto* update = InstOf(value);
	const auto  tag    = "row_shr:" + std::to_string(shift) + " step: ";
	if (update == nullptr || update->GetOpcode() != ValueOpcode::DppUpdateU32) {
		g_diag.Note(5, tag + "chain value is " + OpName(value) + ", not DppUpdateU32");
		return std::nullopt;
	}
	const auto flags = update->Flags<DppMoveFlags>();
	if (flags.control != 0x110u + shift || flags.row_mask != 0xfu || flags.bank_mask != 0xfu ||
	    flags.fetch_inactive || flags.bound_control || flags.dpp8 || !IsAllTrue(update->Arg(2))) {
		g_diag.Note(5, tag + "DppUpdate mismatch ctrl=" + std::to_string(flags.control) +
		                   " row_mask=" + std::to_string(flags.row_mask) +
		                   " bank_mask=" + std::to_string(flags.bank_mask) +
		                   " fi=" + std::to_string(flags.fetch_inactive) +
		                   " bc=" + std::to_string(flags.bound_control) +
		                   " dpp8=" + std::to_string(flags.dpp8) +
		                   " exec_all_true=" + std::to_string(IsAllTrue(update->Arg(2))) +
		                   " exec_op=" + OpName(update->Arg(2)));
		return std::nullopt;
	}
	const auto  y  = update->Arg(1).Resolve();
	const auto* op = InstOf(update->Arg(0));
	if (op == nullptr || op->GetOpcode() != opcode) {
		g_diag.Note(5, tag + "combine op is " + OpName(update->Arg(0)) + ", expected " +
		                   std::string(ValueOpcodeName(opcode)));
		return std::nullopt;
	}
	for (size_t i = 0; i < 2; i++) {
		const auto* move = InstOf(op->Arg(i));
		if (move == nullptr || move->GetOpcode() != ValueOpcode::DppMoveU32) continue;
		const auto move_flags = move->Flags<DppMoveFlags>();
		if (move_flags.control == flags.control && move_flags.row_mask == 0xfu &&
		    move_flags.bank_mask == 0xfu && !move_flags.fetch_inactive && !move_flags.dpp8 &&
		    move->Arg(0).Resolve() == y && IsAllTrue(move->Arg(1)) &&
		    op->Arg(1 - i).Resolve() == y) {
			return y;
		}
	}
	g_diag.Note(5, tag + "no matching DppMove(y) operand: ops " + OpName(op->Arg(0)) + "," +
	                   OpName(op->Arg(1)));
	return std::nullopt;
}

std::optional<Reduction> MatchReduction(Value source) {
	// Final combine: Select(E, Op(P, Q), P) (or just Op(P, Q)).
	const auto* combine = StripAllTrueSelect(source).TryInstruction();
	if (combine == nullptr) {
		g_diag.Note(0, "readlane source is not an instruction (" + OpName(source) + ")");
		return std::nullopt;
	}
	const auto op = ReduceOpOf(combine->GetOpcode());
	if (!op) {
		g_diag.Note(0, "readlane source opcode " + std::string(ValueOpcodeName(combine->GetOpcode())) +
		                   " is not a reduction combine");
		return std::nullopt;
	}

	// Q = V_PERMLANEX16(P, -1, -1): every lane reads lane 15 of the opposite row of its pair.
	for (size_t i = 0; i < 2; i++) {
		const auto* permlane = StripAllTrueSelect(combine->Arg(i)).TryInstruction();
		if (permlane == nullptr || permlane->GetOpcode() != ValueOpcode::Permlane16U32) {
			g_diag.Note(1, "combine(" + std::string(ValueOpcodeName(combine->GetOpcode())) +
			                   ") operand " + std::to_string(i) + " is " + OpName(combine->Arg(i)) +
			                   ", not Permlane16");
			continue;
		}
		const auto flags = permlane->Flags<PermlaneFlags>();
		const auto sel0  = permlane->Arg(1).Resolve();
		const auto sel1  = permlane->Arg(2).Resolve();
		if (!flags.x16 || flags.fetch_inactive || !sel0.IsImmediate() || sel0.U32() != ~0u ||
		    !sel1.IsImmediate() || sel1.U32() != ~0u || !IsAllTrue(permlane->Arg(3))) {
			g_diag.Note(2, "permlane mismatch x16=" + std::to_string(flags.x16) +
			                   " fi=" + std::to_string(flags.fetch_inactive) +
			                   " sel0=" + (sel0.IsImmediate() ? std::to_string(sel0.U32()) : OpName(sel0)) +
			                   " sel1=" + (sel1.IsImmediate() ? std::to_string(sel1.U32()) : OpName(sel1)) +
			                   " exec_all_true=" + std::to_string(IsAllTrue(permlane->Arg(3))) +
			                   " exec_op=" + OpName(permlane->Arg(3)));
			continue;
		}
		const auto p = combine->Arg(1 - i).Resolve();
		if (!(permlane->Arg(0).Resolve() == p)) {
			g_diag.Note(3, "permlane input (" + OpName(permlane->Arg(0)) +
			                   ") differs from the other combine operand (" + OpName(p) + ")");
			continue;
		}

		// P = four row_shr steps 1, 2, 4, 8 (innermost first) over the neutral-filled input.
		const auto opcode = combine->GetOpcode();
		Value      cursor = p;
		bool       ok     = true;
		for (const uint32_t shift: {8u, 4u, 2u, 1u}) {
			const auto next = MatchRowShrStep(cursor, opcode, shift);
			if (!next) {
				ok = false;
				break;
			}
			cursor = *next;
		}
		if (!ok) continue;

		// input = Select(c, V, neutral) where c is bounded by EXEC: lanes that were not
		// launched read the neutral element on hardware.
		g_whole_region = nullptr;
		if (const auto* outer = InstOf(cursor);
		    outer != nullptr && outer->GetOpcode() == ValueOpcode::SelectU32 &&
		    IsAllTrue(outer->Arg(0))) {
			g_whole_region = InstOf(outer->Arg(0));
		}
		const auto  input = StripAllTrueSelect(cursor);
		const auto* fill  = input.TryInstruction();
		if (fill != nullptr && fill->GetOpcode() == ValueOpcode::SelectU32) {
			const auto                      neutral = fill->Arg(2).Resolve();
			std::unordered_set<const Inst*> visiting;
			if (!neutral.IsImmediate() || neutral.GetType() != Type::U32 ||
			    neutral.U32() != NeutralOf(*op) || !IsExecBounded(fill->Arg(0), visiting)) {
				const auto bounded = [](Value v) {
					std::unordered_set<const Inst*> fresh;  // `visiting` is polluted after the first query
					return IsExecBounded(v, fresh);
				};
				const auto* cond = InstOf(fill->Arg(0));
				std::string operands;
				if (cond != nullptr && cond->GetOpcode() != ValueOpcode::Phi) {
					for (size_t i = 0; i < cond->NumArgs() && i < 2; i++) {
						operands += " arg" + std::to_string(i) + "=" + OpName(cond->Arg(i)) + "/" +
						            std::to_string(bounded(cond->Arg(i)));
					}
				}
				g_diag.Note(6, "neutral fill rejected: neutral=" +
				                   (neutral.IsImmediate() ? std::to_string(neutral.U32())
				                                          : OpName(neutral)) +
				                   " expected=" + std::to_string(NeutralOf(*op)) +
				                   " cond=" + OpName(fill->Arg(0)) +
				                   " cond_exec_bounded=" + std::to_string(bounded(fill->Arg(0))) +
				                   operands);
				continue;
			}
		}
		// Otherwise there is no Select at all: constant propagation of the initial EXEC (a literal
		// true) folded away `V_CNDMASK(neutral, V, exec & k)` with a provably true k (ps 33037c60,
		// pc 0x21f8: s28 = exec & s80). The DPP chain still reads only existing lanes, so the
		// subgroup reduction over existing invocations is the same value.
		return Reduction {*op, input};
	}
	return std::nullopt;
}

} // namespace

PartialWaveReductionStats LowerPartialWaveReductions(Program& program) {
	PartialWaveReductionStats stats;
	// Only host pixel waves are partial; compute/vertex waves are launched whole.
	if (program.stage != ShaderType::Pixel ||
	    (program.wave_size != 32u && program.wave_size != 64u)) {
		return stats;
	}
	std::vector<Inst*> reads;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::ReadLane) continue;
			const auto selector = inst.Arg(1).Resolve();
			if (!selector.IsImmediate() || selector.GetType() != Type::U32) continue;
			const auto lane = selector.U32();
			if (lane != 31u && !(lane == 63u && program.wave_size == 64u)) continue;
			reads.push_back(&inst);
		}
	}
	for (auto* read: reads) {
		const auto lane      = read->Arg(1).Resolve().U32();
		g_diag = {};
		const auto reduction = MatchReduction(read->Arg(0));
		if (!reduction) {
			stats.rejections.push_back("readlane=" + std::to_string(lane) + " reason=" +
			                           (g_diag.text.empty() ? std::string("unknown") : g_diag.text));
			continue;
		}

		auto* block = read->Parent();
		auto  where = block->begin();
		while (&*where != read) ++where;
		const auto emit = [&](ValueOpcode opcode, std::initializer_list<Value> args,
		                      uint64_t flags = 0) {
			return Value(&*block->PrependNewInst(where, opcode, args, flags));
		};
		Value filled = reduction->input;
		if (program.wave_size == 64u) {
			// V_READLANE 31 sees lanes 0..31, 63 sees 32..63.
			const auto lane_id = emit(ValueOpcode::LaneId, {});
			const auto half    = emit(ValueOpcode::ShiftRightLogical32, {lane_id, Value(5u)});
			const auto in_half = emit(ValueOpcode::IEqual32, {half, Value(lane >> 5)});
			filled             = emit(ValueOpcode::SelectU32,
			                          {in_half, reduction->input, Value(NeutralOf(reduction->op))});
		}
		const auto reduced = emit(ValueOpcode::WaveReduceU32, {filled},
		                          static_cast<uint64_t>(static_cast<uint32_t>(reduction->op)));
		read->ReplaceUsesWith(reduced);
		stats.rewritten_reads++;
	}
	return stats;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
