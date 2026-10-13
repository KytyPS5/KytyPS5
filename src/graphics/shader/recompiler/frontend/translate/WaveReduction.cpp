#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Frontend {
namespace {

using Decoder::Instruction;
using Decoder::Operand;
using Decoder::OperandKind;

bool PlainVgpr(const Operand& operand) {
	return operand.kind == OperandKind::Vgpr && !operand.negate && !operand.absolute &&
	       !operand.dpp && !operand.dpp8 && operand.sdwa_sel == 6u && !operand.sdwa_sext;
}

bool PlainVgpr(const Operand& operand, uint32_t reg) {
	return PlainVgpr(operand) && operand.reg == reg;
}

bool PlainDestination(const Instruction& inst) {
	const auto& dst = inst.dst;
	return dst.kind == OperandKind::Vgpr && !dst.clamp && dst.omod == 0u &&
	       !dst.explicit_sdwa_dst && dst.sdwa_sel == 6u;
}

bool IsAllOnes(const Operand& operand) {
	return (operand.kind == OperandKind::IntegerInlineConstant && operand.signed_val == -1) ||
	       (operand.kind == OperandKind::LiteralConstant && operand.value == 0xffffffffu);
}

bool WritesVgpr(const Instruction& inst, uint32_t reg) {
	if (inst.opcode == Decoder::Opcode::V_MOVRELD_B32) {
		return true;
	}
	// Overestimate the width: a false match only drops the pattern.
	const auto width = std::max(inst.data_dwords, 2u) + 1u;
	for (const auto* dst: {&inst.dst, &inst.dst2}) {
		if (dst->kind == OperandKind::Vgpr && reg >= dst->reg && reg < dst->reg + width) {
			return true;
		}
	}
	return false;
}

bool IsLeaf(const Instruction& inst, const LaunchedMasks& masks) {
	if (inst.opcode != Decoder::Opcode::V_CNDMASK_B32 || !PlainDestination(inst)) {
		return false;
	}
	Operand mask;
	mask.kind = OperandKind::VccLo;
	if (inst.src_count >= 3u) {
		mask = inst.src2;
	}
	const auto& zero = inst.src0;
	return zero.kind == OperandKind::IntegerInlineConstant && zero.value == 0u &&
	       zero.signed_val == 0 && !zero.negate && !zero.absolute && PlainVgpr(inst.src1) &&
	       masks.Launched(mask);
}

bool IsRowShiftOr(const Instruction& inst, uint32_t x, uint32_t shift) {
	const auto& src = inst.src0;
	return inst.opcode == Decoder::Opcode::V_OR_B32 && PlainDestination(inst) &&
	       inst.dst.reg == x && src.kind == OperandKind::Vgpr && src.reg == x && src.dpp &&
	       !src.dpp8 && src.dpp_ctrl == 0x110u + shift && src.dpp_row_mask == 0xfu &&
	       src.dpp_bank_mask == 0xfu && !src.negate && !src.absolute && PlainVgpr(inst.src1, x);
}

bool IsOppositeRowLast(const Instruction& inst, uint32_t x) {
	return inst.opcode == Decoder::Opcode::V_PERMLANEX16_B32 &&
	       inst.dst.kind == OperandKind::Vgpr && inst.dst.reg != x && PlainVgpr(inst.src0, x) &&
	       IsAllOnes(inst.src1) && IsAllOnes(inst.src2);
}

bool IsMerge(const Instruction& inst, uint32_t x, uint32_t t) {
	return inst.opcode == Decoder::Opcode::V_OR_B32 && PlainDestination(inst) &&
	       inst.dst.reg == x &&
	       ((PlainVgpr(inst.src0, x) && PlainVgpr(inst.src1, t)) ||
	        (PlainVgpr(inst.src0, t) && PlainVgpr(inst.src1, x)));
}

} // namespace

// With EXEC all ones, every lane of the wave runs this sequence, including lanes the host
// subgroup does not contain. A leaf `v_cndmask x, 0, v, m` whose mask has no bit for those
// lanes makes them contribute zero, so the scan is defined by the launched lanes alone.
void Translator::TrackWaveOrScan(const Instruction& inst, const LaunchedMasks& before) {
	if (!before.exec_full) {
		wave_or_scan.reset();
		return;
	}
	if (IsLeaf(inst, before)) {
		const auto x = inst.dst.reg;
		wave_or_scan =
		    WaveOrScan {.x = x, .step = 0, .leaf = ir.GetVectorReg(static_cast<IR::VectorReg>(x))};
		return;
	}
	if (!wave_or_scan) {
		return;
	}
	auto& scan = *wave_or_scan;
	if (scan.step < 4u && IsRowShiftOr(inst, scan.x, 1u << scan.step)) {
		scan.step++;
		return;
	}
	if (scan.step == 4u && IsOppositeRowLast(inst, scan.x)) {
		scan.t    = inst.dst.reg;
		scan.step = 5u;
		ir.SetVectorReg(static_cast<IR::VectorReg>(scan.t),
		                IR::U32(ir.Emit(IR::ValueOpcode::WaveOrPatternU32, {scan.leaf},
		                                IR::WaveOrPattern::OppositeRow)));
		return;
	}
	if (scan.step == 5u && IsMerge(inst, scan.x, scan.t)) {
		ir.SetVectorReg(static_cast<IR::VectorReg>(scan.x),
		                IR::U32(ir.Emit(IR::ValueOpcode::WaveOrPatternU32, {scan.leaf},
		                                IR::WaveOrPattern::RowPairScan)));
		wave_or_scan.reset();
		return;
	}
	if (WritesVgpr(inst, scan.x) || (scan.step == 5u && WritesVgpr(inst, scan.t))) {
		wave_or_scan.reset();
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
