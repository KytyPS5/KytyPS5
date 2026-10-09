#include "graphics/shader/recompiler/ir/passes/SelectForwarding.h"

#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {

// Results depend only on operands of the same lane, with no side effect or fault.
bool IsLaneLocal(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::BitCastU16F16:
		case ValueOpcode::BitCastF16U16:
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::BitCastU64F64:
		case ValueOpcode::BitCastF64U64:
		case ValueOpcode::ConvertU16U32:
		case ValueOpcode::ConvertU32U16:
		case ValueOpcode::ConvertU8U32:
		case ValueOpcode::ConvertU32U8:
		case ValueOpcode::ConvertF32F16:
		case ValueOpcode::ConvertF16F32:
		case ValueOpcode::ConvertS32F32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32S32:
		case ValueOpcode::ConvertF64S32:
		case ValueOpcode::ConvertF32F64:
		case ValueOpcode::ConvertF64F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::ConvertF64U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeConstructU32x3:
		case ValueOpcode::CompositeConstructF32x2:
		case ValueOpcode::CompositeConstructU32x4:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::CompositeExtractU32x3:
		case ValueOpcode::CompositeExtractU32x4:
		case ValueOpcode::PackHalf2x16:
		case ValueOpcode::PackSnorm2x16:
		case ValueOpcode::PackUnorm2x16:
		case ValueOpcode::PackFloat2x16Rtz:
		case ValueOpcode::FPAbs32:
		case ValueOpcode::FPNeg32:
		case ValueOpcode::FPSaturate32:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectF32:
		case ValueOpcode::SelectU32:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::IAddCarry32:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::IMul64:
		case ValueOpcode::SMulHi:
		case ValueOpcode::UMulHi:
		case ValueOpcode::IAbs32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::BitReverse32:
		case ValueOpcode::BitCount32:
		case ValueOpcode::BitCount64:
		case ValueOpcode::FindUMsb32:
		case ValueOpcode::FindUMsb64:
		case ValueOpcode::FindILsb32:
		case ValueOpcode::SMin32:
		case ValueOpcode::UMin32:
		case ValueOpcode::SMax32:
		case ValueOpcode::UMax32:
		case ValueOpcode::SMinTri32:
		case ValueOpcode::UMinTri32:
		case ValueOpcode::SMaxTri32:
		case ValueOpcode::UMaxTri32:
		case ValueOpcode::SMedTri32:
		case ValueOpcode::UMedTri32:
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThan64:
		case ValueOpcode::SLessThanEqual64:
		case ValueOpcode::ULessThanEqual64:
		case ValueOpcode::UGreaterThanEqual64:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThan64:
		case ValueOpcode::IEqual32:
		case ValueOpcode::IEqual64:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::UGreaterThan64:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::INotEqual64:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdEqual32:
		case ValueOpcode::FPOrdEqual64:
		case ValueOpcode::FPOrdLessThanEqual64:
		case ValueOpcode::FPOrdGreaterThanEqual64:
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
		case ValueOpcode::FPCmpClass32:
		case ValueOpcode::FPCmpClass16:
		case ValueOpcode::FPAdd32:
		case ValueOpcode::FPSub32:
		case ValueOpcode::FPFma32:
		case ValueOpcode::FPMad32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPAdd64:
		case ValueOpcode::FPMul64:
		case ValueOpcode::FPFma64:
		case ValueOpcode::FPRecip64:
		case ValueOpcode::FPMin32:
		case ValueOpcode::FPMax32:
		case ValueOpcode::FPMin64:
		case ValueOpcode::FPMax64:
		case ValueOpcode::FPMinTri32:
		case ValueOpcode::FPMaxTri32:
		case ValueOpcode::FPMedTri32:
		case ValueOpcode::FPRecip32:
		case ValueOpcode::FPRecipIFlag32:
		case ValueOpcode::FPRecipSqrt32:
		case ValueOpcode::FPSqrt:
		case ValueOpcode::FPSin:
		case ValueOpcode::FPCos:
		case ValueOpcode::FPExp2:
		case ValueOpcode::FPLog2:
		case ValueOpcode::FPLdexp:
		case ValueOpcode::FPRoundEven32:
		case ValueOpcode::FPFloor32:
		case ValueOpcode::FPCeil32:
		case ValueOpcode::FPTrunc32:
		case ValueOpcode::FPFloor64:
		case ValueOpcode::FPCeil64:
		case ValueOpcode::FPTrunc64:
		case ValueOpcode::FPFract32:
		case ValueOpcode::FPFract64: return true;
		default: return false;
	}
}

bool IsSelect(ValueOpcode op) {
	return op == ValueOpcode::SelectU1 || op == ValueOpcode::SelectF32 ||
	       op == ValueOpcode::SelectU32;
}

// The condition every use of an instruction is guarded by, or an empty value. A result is
// guarded by c when each use is the selected operand of Select(c, ., .) or a lane-local
// instruction that is itself guarded by c. Users are visited first: in reverse order, a
// non-Phi user follows the definition it reads.
std::unordered_map<const Inst*, Value> ComputeGuards(const BlockList& blocks) {
	std::unordered_map<const Inst*, Value> guards;
	for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
		auto& instructions = (*block)->Instructions();
		for (auto inst = instructions.rbegin(); inst != instructions.rend(); ++inst) {
			if (!IsLaneLocal(inst->GetOpcode()) || !inst->HasUses()) continue;
			Value guard {};
			bool  valid = true;
			for (const auto& use: inst->Uses()) {
				const auto* user = use.user;
				Value       condition {};
				if (user->Parent() != nullptr && IsSelect(user->GetOpcode()) && use.operand == 1) {
					condition = user->Arg(0);
				} else if (user->Parent() != nullptr && IsLaneLocal(user->GetOpcode())) {
					if (const auto found = guards.find(user); found != guards.end()) {
						condition = found->second;
					}
				}
				if (condition.IsEmpty() || (!guard.IsEmpty() && !(guard == condition))) {
					valid = false;
					break;
				}
				guard = condition;
			}
			if (valid) guards.emplace(&*inst, guard);
		}
	}
	return guards;
}

} // namespace

void ForwardGuardedSelects(const BlockList& blocks) {
	for (bool changed = true; changed;) {
		changed           = false;
		const auto guards = ComputeGuards(blocks);
		// Only a select whose every use can be rewritten is forwarded: it then dies, so its true
		// operand replaces it instead of staying live beside it.
		const auto removable = [&](const Inst& select) {
			for (const auto& use: select.Uses()) {
				const auto* user = use.user;
				if (IsSelect(user->GetOpcode()) && user->GetOpcode() == select.GetOpcode() &&
				    use.operand == 2 && user->Arg(0) == select.Arg(0)) {
					continue;
				}
				const auto found = guards.find(user);
				if ((IsSelect(user->GetOpcode()) && use.operand == 0) || found == guards.end() ||
				    !(found->second == select.Arg(0))) {
					return false;
				}
			}
			return true;
		};
		for (auto* block: blocks) {
			for (auto& inst: *block) {
				// Select(c, x, Select(c, y, z)) only reads z through its false operand.
				if (IsSelect(inst.GetOpcode())) {
					const auto* inner = inst.Arg(2).TryInstruction();
					if (inner != nullptr && inner->GetOpcode() == inst.GetOpcode() &&
					    inner->Arg(0) == inst.Arg(0) && removable(*inner)) {
						inst.SetArg(2, inner->Arg(2));
						changed = true;
					}
				}
				const auto found = guards.find(&inst);
				if (found == guards.end()) continue;
				const auto guard = found->second;
				for (size_t arg = IsSelect(inst.GetOpcode()) ? 1 : 0; arg < inst.NumArgs(); ++arg) {
					const auto* select = inst.Arg(arg).TryInstruction();
					if (select != nullptr && IsSelect(select->GetOpcode()) &&
					    select->Arg(0) == guard && removable(*select)) {
						inst.SetArg(arg, select->Arg(1));
						changed = true;
					}
				}
			}
		}
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
