#include "graphics/shader/recompiler/frontend/translate/Translator.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <unordered_map>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

using ShaderError::Fail;

Decoder::Operand Translator::SourceAt(const Decoder::Instruction& inst, uint32_t index) {
	switch (index) {
		case 0: return inst.src0;
		case 1: return inst.src1;
		case 2: return inst.src2;
		case 3: return inst.src3;
		default: EXIT("decoded source operand index is out of range");
	}
}

Decoder::Operand Translator::DestinationOperand(const Decoder::Instruction& inst) {
	auto destination = inst.dst;
	if (destination.kind != Decoder::OperandKind::Vgpr) {
		return destination;
	}
	for (uint32_t index = 0; index < std::min(inst.src_count, 3u); index++) {
		const auto source = SourceAt(inst, index);
		if (!source.dpp) {
			continue;
		}
		destination.dpp                = true;
		destination.dpp_ctrl           = source.dpp_ctrl;
		destination.dpp_row_mask       = source.dpp_row_mask;
		destination.dpp_bank_mask      = source.dpp_bank_mask;
		destination.dpp_fetch_inactive = source.dpp_fetch_inactive;
		destination.dpp_bound_ctrl     = source.dpp_bound_ctrl;
		break;
	}
	return destination;
}

Decoder::Operand Translator::OffsetOperand(const Decoder::Operand& operand, uint32_t offset) {
	if (offset == 0) {
		return operand;
	}
	auto result = PlainOperand(operand);
	switch (result.kind) {
		case Decoder::OperandKind::Sgpr:
		case Decoder::OperandKind::Vgpr: result.reg += offset; break;
		case Decoder::OperandKind::VccLo:
			EXIT_IF(offset != 1u);
			result.kind = Decoder::OperandKind::VccHi;
			break;
		case Decoder::OperandKind::ExecLo:
			EXIT_IF(offset != 1u);
			result.kind = Decoder::OperandKind::ExecHi;
			break;
		case Decoder::OperandKind::VccHi:
		case Decoder::OperandKind::ExecHi: EXIT("special-register operand offset is out of range");
		default: return operand;
	}
	return result;
}

Decoder::Operand Translator::ScalarDestinationOperand(const Decoder::Operand& operand,
                                                      uint32_t                offset) {
	uint32_t code = 0;
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: code = operand.reg; break;
		case Decoder::OperandKind::VccLo: code = 106u; break;
		case Decoder::OperandKind::VccHi: code = 107u; break;
		default: EXIT("invalid scalar-memory destination");
	}
	code += offset;
	Decoder::Operand result {};
	if (code < 106u) {
		result.kind = Decoder::OperandKind::Sgpr;
		result.reg  = code;
	} else {
		switch (code) {
			case 106u: result.kind = Decoder::OperandKind::VccLo; break;
			case 107u: result.kind = Decoder::OperandKind::VccHi; break;
			default: EXIT("scalar-memory destination crosses an invalid register");
		}
	}
	return result;
}

Decoder::Operand Translator::PlainOperand(const Decoder::Operand& operand) {
	auto result               = operand;
	result.sdwa_sel           = 6;
	result.sdwa_dst_unused    = 2;
	result.omod               = 0;
	result.sdwa_sext          = false;
	result.op_sel             = false;
	result.op_sel_hi          = false;
	result.negate             = false;
	result.negate_hi          = false;
	result.absolute           = false;
	result.clamp              = false;
	result.dpp_ctrl           = 0;
	result.dpp_row_mask       = 0xf;
	result.dpp_bank_mask      = 0xf;
	result.dpp_fetch_inactive = false;
	result.dpp_bound_ctrl     = false;
	result.dpp                = false;
	return result;
}

std::array<IR::U32, 2> Translator::BallotMask(IR::U1 value) {
	return {ir.Select(value, IR::U32(IR::Value(1u)), IR::U32(IR::Value(0u))),
	        IR::U32(IR::Value(0u))};
}

IR::U32 Translator::ReadRawU32(const Decoder::Operand& operand) {
	switch (operand.kind) {
		case Decoder::OperandKind::LiteralConstant:
		case Decoder::OperandKind::IntegerInlineConstant:
		case Decoder::OperandKind::FloatInlineConstant: return IR::U32(IR::Value(operand.value));
		case Decoder::OperandKind::Null:
		case Decoder::OperandKind::PopsExitingWaveId: return IR::U32(IR::Value(0u));
		case Decoder::OperandKind::Sgpr:
			return ir.GetScalarReg(static_cast<IR::ScalarReg>(operand.reg));
		case Decoder::OperandKind::Vgpr:
			return ir.GetVectorReg(static_cast<IR::VectorReg>(operand.reg));
		case Decoder::OperandKind::VccLo: return ir.GetVccLo();
		case Decoder::OperandKind::VccHi: return ir.GetVccHi();
		case Decoder::OperandKind::M0: return ir.GetM0();
		case Decoder::OperandKind::ExecLo: return ir.GetExecLo();
		case Decoder::OperandKind::ExecHi: return ir.GetExecHi();
		case Decoder::OperandKind::Scc:
			return ir.Select(ir.GetScc(), IR::U32(IR::Value(1u)), IR::U32(IR::Value(0u)));
		case Decoder::OperandKind::VccZ:
			return ir.Select(ir.LogicalNot(ir.GetVcc()), IR::U32(IR::Value(1u)),
			                 IR::U32(IR::Value(0u)));
		case Decoder::OperandKind::ExecZ:
			return ir.Select(ir.LogicalNot(ir.GetExec()), IR::U32(IR::Value(1u)),
			                 IR::U32(IR::Value(0u)));
		default: EXIT("invalid decoded operand used as a raw U32 source");
	}
}

// Scalar operands share one encoded namespace with VCC, M0, and EXEC aliases.
IR::U32 Translator::ReadScalarCode(uint32_t code) {
	if (code < 106u) {
		return ir.GetScalarReg(static_cast<IR::ScalarReg>(code));
	}
	switch (code) {
		case 106u: return ir.GetVccLo();
		case 107u: return ir.GetVccHi();
		case 124u: return ir.GetM0();
		case 126u:
		case 127u: {
			const auto mask = BallotMask(ir.GetExec());
			return mask[code - 126u];
		}
		default: return IR::U32(IR::Value(0u));
	}
}

IR::U32 Translator::ApplyBitSourceModifiers(const Decoder::Operand& operand, IR::U32 value) {
	if (operand.dpp) {
		const IR::DppMoveFlags flags {
		    .control        = static_cast<uint16_t>(operand.dpp_ctrl),
		    .row_mask       = static_cast<uint8_t>(operand.dpp_row_mask),
		    .bank_mask      = static_cast<uint8_t>(operand.dpp_bank_mask),
		    .fetch_inactive = operand.dpp_fetch_inactive,
		    .bound_control  = operand.dpp_bound_ctrl,
		};
		value = IR::U32(ir.Emit(IR::ValueOpcode::DppMoveU32, {value, ir.GetExec()}, flags));
	}
	if (operand.sdwa_sel != 6u) {
		uint32_t offset = 0;
		uint32_t width  = 0;
		if (operand.sdwa_sel <= 3u) {
			offset = operand.sdwa_sel * 8u;
			width  = 8u;
		} else if (operand.sdwa_sel == 4u || operand.sdwa_sel == 5u) {
			offset = operand.sdwa_sel == 5u ? 16u : 0u;
			width  = 16u;
		} else {
			EXIT("invalid SDWA source selector");
		}
		const auto opcode = operand.sdwa_sext ? IR::ValueOpcode::BitFieldSExtract
		                                      : IR::ValueOpcode::BitFieldUExtract;
		value             = IR::U32(ir.Emit(opcode, {value, IR::Value(offset), IR::Value(width)}));
	}
	return value;
}

IR::Value Translator::ReadOperand(const Decoder::Operand& operand, IR::Type type) {
	if (type == IR::Type::U16) {
		return ir.Emit(IR::ValueOpcode::ConvertU16U32,
		               {ApplyBitSourceModifiers(operand, ReadRawU32(operand))});
	}
	if (type == IR::Type::F16) {
		const auto bits = IR::U16(ir.Emit(IR::ValueOpcode::ConvertU16U32,
		                                  {ApplyBitSourceModifiers(operand, ReadRawU32(operand))}));
		return ir.Emit(IR::ValueOpcode::BitCastF16U16, {bits});
	}
	if (type == IR::Type::U1) {
		switch (operand.kind) {
			case Decoder::OperandKind::Scc: return ir.GetScc();
			case Decoder::OperandKind::ExecLo:
			case Decoder::OperandKind::ExecHi: return ir.GetExec();
			case Decoder::OperandKind::VccLo:
			case Decoder::OperandKind::VccHi: return ir.GetVcc();
			case Decoder::OperandKind::VccZ: return ir.LogicalNot(ir.GetVcc());
			case Decoder::OperandKind::ExecZ: return ir.LogicalNot(ir.GetExec());
			default: break;
		}
		return ir.INotEqual(ReadRawU32(operand), IR::U32(IR::Value(0u)));
	}
	if (type == IR::Type::U64) {
		const auto pair = ReadU32Pair(operand);
		return ir.ConstructU64(pair[0], pair[1]);
	}
	auto bits = ApplyBitSourceModifiers(operand, ReadRawU32(operand));
	if (TypesOverlap(type, IR::Type::F32) && !TypesOverlap(type, IR::Type::U32)) {
		auto value = ir.BitCastF32(bits);
		if (operand.absolute) {
			value = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {value}));
		}
		if (operand.negate) {
			value = IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {value}));
		}
		return value;
	}
	if (operand.absolute) {
		bits = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x7fffffffu)));
	}
	if (operand.negate) {
		bits = ir.BitwiseXor(bits, IR::U32(IR::Value(0x80000000u)));
	}
	if (!TypesOverlap(type, IR::Type::U32)) {
		EXIT("opcode %u at 0x%08x requested unsupported operand type %s",
		     static_cast<uint32_t>(current_opcode), current_pc, IR::TypeName(type).c_str());
	}
	return bits;
}

void Translator::WriteRawU32(const Decoder::Operand& operand, IR::U32 value) {
	if (operand.kind == Decoder::OperandKind::Null) {
		return;
	}
	if (operand.sdwa_sel != 6u) {
		uint32_t offset = 0;
		uint32_t width  = 0;
		if (operand.sdwa_sel <= 3u) {
			offset = operand.sdwa_sel * 8u;
			width  = 8u;
		} else if (operand.sdwa_sel == 4u || operand.sdwa_sel == 5u) {
			offset = operand.sdwa_sel == 5u ? 16u : 0u;
			width  = 16u;
		} else {
			EXIT("invalid SDWA destination selector");
		}
		switch (operand.sdwa_dst_unused) {
			case 0:
				value =
				    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldInsert,
				                    {IR::Value(0u), value, IR::Value(offset), IR::Value(width)}));
				break;
			case 1: {
				const auto extended = IR::U32(ir.Emit(IR::ValueOpcode::BitFieldSExtract,
				                                      {value, IR::Value(0u), IR::Value(width)}));
				value               = ir.ShiftLeftLogical(extended, IR::U32(IR::Value(offset)));
				break;
			}
			case 2: {
				const uint32_t field_mask = width == 32u ? 0xffffffffu : (1u << width) - 1u;
				const auto     inserted =
				    ir.ShiftLeftLogical(ir.BitwiseAnd(value, IR::U32(IR::Value(field_mask))),
				                        IR::U32(IR::Value(offset)));
				const auto cleared = ir.BitwiseAnd(ReadRawU32(PlainOperand(operand)),
				                                   IR::U32(IR::Value(~(field_mask << offset))));
				value              = ir.BitwiseOr(cleared, inserted);
			} break;
			default: EXIT("reserved SDWA DST_U mode");
		}
	}
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: {
			const auto reg = static_cast<IR::ScalarReg>(operand.reg);
			ir.SetScalarReg(reg, value);
			ir.SetThreadBitScalarReg(reg, ThreadBit(value));
			ir.SetScalarMaskTag(reg, IR::U1(IR::Value(false)));
			if (IR::RegIndex(reg) > 0u) {
				ir.SetScalarMaskTag(static_cast<IR::ScalarReg>(IR::RegIndex(reg) - 1u),
				                    IR::U1(IR::Value(false)));
			}
			break;
		}
		case Decoder::OperandKind::Vgpr: {
			const auto reg = static_cast<IR::VectorReg>(operand.reg);
			const auto old = ir.GetVectorReg(reg);
			if (operand.dpp) {
				const IR::DppMoveFlags flags {
				    .control        = static_cast<uint16_t>(operand.dpp_ctrl),
				    .row_mask       = static_cast<uint8_t>(operand.dpp_row_mask),
				    .bank_mask      = static_cast<uint8_t>(operand.dpp_bank_mask),
				    .fetch_inactive = operand.dpp_fetch_inactive,
				    .bound_control  = operand.dpp_bound_ctrl,
				};
				value = IR::U32(
				    ir.Emit(IR::ValueOpcode::DppUpdateU32, {value, old, ir.GetExec()}, flags));
			} else {
				value = ir.Select(ir.GetExec(), value, old);
			}
			ir.SetVectorReg(reg, value);
			break;
		}
		case Decoder::OperandKind::VccLo:
			ir.SetVccLo(IR::U32(value));
			ir.SetVcc(ThreadBit(IR::U32(value)));
			break;
		case Decoder::OperandKind::VccHi:
			ir.SetVccHi(IR::U32(value));
			ir.SetVcc(ThreadBit(ir.GetVccLo()));
			break;
		case Decoder::OperandKind::M0: ir.SetM0(IR::U32(value)); break;
		case Decoder::OperandKind::ExecLo:
			ir.SetExecLo(IR::U32(value));
			ir.SetExec(ThreadBit(IR::U32(value)));
			break;
		case Decoder::OperandKind::ExecHi:
			ir.SetExecHi(IR::U32(value));
			ir.SetExec(ThreadBit(ir.GetExecLo()));
			break;
		case Decoder::OperandKind::Scc:
			ir.SetScc(ir.INotEqual(value, IR::U32(IR::Value(0u))));
			break;
		default: EXIT("invalid decoded operand used as a destination");
	}
}

IR::F32 Translator::ApplyF32ResultModifiers(const Decoder::Operand& operand, IR::F32 value) {
	if (operand.omod != 0u) {
		float multiplier = 0.5f;
		switch (operand.omod) {
			case 1u: multiplier = 2.0f; break;
			case 2u: multiplier = 4.0f; break;
			default: break;
		}
		value = IR::F32(ir.Emit(IR::ValueOpcode::FPMul32, {value, IR::Value::F32(multiplier)}));
	}
	if (operand.clamp) {
		value = IR::F32(ir.Emit(IR::ValueOpcode::FPSaturate32, {value}));
	}
	return value;
}

void Translator::WriteOperand(const Decoder::Operand& operand, IR::Value value) {
	if (operand.kind == Decoder::OperandKind::Null) {
		return;
	}
	auto type = value.GetType();
	if (type == IR::Type::F32) {
		value = ApplyF32ResultModifiers(operand, IR::F32(value));
		type  = IR::Type::F32;
	}
	if (type == IR::Type::Opaque) {
		EXIT("opcode %u at 0x%08x produced an untyped value", static_cast<uint32_t>(current_opcode),
		     current_pc);
	}
	if (type == IR::Type::U1) {
		switch (operand.kind) {
			case Decoder::OperandKind::Scc: ir.SetScc(IR::U1(value)); return;
			case Decoder::OperandKind::ExecLo:
			case Decoder::OperandKind::ExecHi: {
				const auto mask = BallotMask(IR::U1(value));
				ir.SetExec(IR::U1(value));
				ir.SetExecLo(mask[0]);
				ir.SetExecHi(mask[1]);
				return;
			}
			case Decoder::OperandKind::VccLo:
			case Decoder::OperandKind::VccHi: {
				const auto mask = BallotMask(IR::U1(value));
				ir.SetVcc(IR::U1(value));
				ir.SetVccLo(mask[0]);
				ir.SetVccHi(mask[1]);
				return;
			}
			default:
				WriteRawU32(operand, ir.Select(IR::U1(value), IR::U32(IR::Value(1u)),
				                               IR::U32(IR::Value(0u))));
				return;
		}
	}
	if (type == IR::Type::U16) {
		WriteRawU32(operand, IR::U32(ir.Emit(IR::ValueOpcode::ConvertU32U16, {IR::U16(value)})));
		return;
	}
	if (type == IR::Type::F16) {
		const auto bits = IR::U16(ir.Emit(IR::ValueOpcode::BitCastU16F16, {value}));
		WriteRawU32(operand, IR::U32(ir.Emit(IR::ValueOpcode::ConvertU32U16, {bits})));
		return;
	}
	if (type == IR::Type::U64) {
		WriteU32Pair(operand, {ir.CompositeExtract(value, 0), ir.CompositeExtract(value, 1)});
		return;
	}
	if (type == IR::Type::F32) {
		WriteRawU32(operand, ir.BitCastU32(IR::F32(value)));
		return;
	}
	EXIT_IF(type != IR::Type::U32);
	WriteRawU32(operand, IR::U32(value));
}

IR::U32 Translator::PackHalf2x16(IR::F32 low, IR::F32 high) {
	const auto pair = ir.Emit(IR::ValueOpcode::CompositeConstructF32x2, {low, high});
	return IR::U32(ir.Emit(IR::ValueOpcode::PackHalf2x16, {pair}));
}

void Translator::WriteF16(const Decoder::Operand& operand, IR::F32 value) {
	value           = ApplyF32ResultModifiers(operand, value);
	const auto half = IR::F16(ir.Emit(IR::ValueOpcode::ConvertF16F32, {value}));
	const auto bits = IR::U32(ir.Emit(IR::ValueOpcode::ConvertU32U16,
	                                  {IR::U16(ir.Emit(IR::ValueOpcode::BitCastU16F16, {half}))}));
	auto       merged = bits;
	if (operand.sdwa_sel == 4u || operand.sdwa_sel == 5u) {
		merged = IR::U32(ir.Emit(IR::ValueOpcode::BitFieldInsert,
		                         {ReadRawU32(PlainOperand(operand)), bits,
		                          IR::Value(operand.sdwa_sel == 5u ? 16u : 0u), IR::Value(16u)}));
	}
	auto raw     = operand;
	raw.sdwa_sel = 6u;
	raw.omod     = 0u;
	raw.op_sel   = false;
	raw.clamp    = false;
	WriteRawU32(raw, merged);
}

void Translator::WriteU16(const Decoder::Operand& operand, IR::U32 value) {
	auto merged = ir.BitwiseAnd(value, IR::U32(IR::Value(0xffffu)));
	if (operand.sdwa_sel == 4u || operand.sdwa_sel == 5u) {
		merged = IR::U32(ir.Emit(IR::ValueOpcode::BitFieldInsert,
		                         {ReadRawU32(PlainOperand(operand)), merged,
		                          IR::Value(operand.sdwa_sel == 5u ? 16u : 0u), IR::Value(16u)}));
	}
	auto raw     = operand;
	raw.sdwa_sel = 6u;
	raw.omod     = 0u;
	raw.op_sel   = false;
	raw.clamp    = false;
	WriteRawU32(raw, merged);
}

IR::U32 Translator::ReadU32(const Decoder::Operand& operand) {
	return IR::U32(ReadOperand(operand, IR::Type::U32));
}

std::array<IR::U32, 2> Translator::ReadU32Pair(const Decoder::Operand& operand) {
	if (operand.kind == Decoder::OperandKind::ExecLo) {
		return {ir.GetExecLo(), ir.GetExecHi()};
	}
	if (operand.kind == Decoder::OperandKind::VccLo) {
		return {ir.GetVccLo(), ir.GetVccHi()};
	}
	const auto low = ApplyBitSourceModifiers(operand, ReadRawU32(operand));
	IR::U32    high(IR::Value(0u));
	if (operand.kind == Decoder::OperandKind::Sgpr || operand.kind == Decoder::OperandKind::Vgpr) {
		high = ReadRawU32(OffsetOperand(operand, 1));
	} else if (operand.kind == Decoder::OperandKind::IntegerInlineConstant &&
	           operand.signed_val < 0) {
		high = IR::U32(IR::Value(0xffffffffu));
	}
	return {low, high};
}

IR::U64 Translator::ReadU64(const Decoder::Operand& operand) {
	return IR::U64(ReadOperand(operand, IR::Type::U64));
}

IR::F32 Translator::ReadF16LaneAsF32(const Decoder::Operand& operand, bool high_lane, bool packed) {
	if (operand.kind == Decoder::OperandKind::FloatInlineConstant) {
		const bool use_zero = packed && (high_lane ? operand.op_sel_hi : operand.op_sel);
		auto       value    = use_zero ? IR::F32(IR::Value::F32(0.0f))
		                               : ir.BitCastF32(IR::U32(IR::Value(operand.value)));
		const auto half     = IR::F16(ir.Emit(IR::ValueOpcode::ConvertF16F32, {value}));
		value               = IR::F32(ir.Emit(IR::ValueOpcode::ConvertF32F16, {half}));
		if (operand.absolute) {
			value = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {value}));
		}
		if (high_lane ? operand.negate_hi : operand.negate) {
			value = IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {value}));
		}
		return value;
	}
	auto raw_operand      = operand;
	raw_operand.sdwa_sel  = 6;
	raw_operand.sdwa_sext = false;
	const auto bits       = ApplyBitSourceModifiers(raw_operand, ReadRawU32(operand));
	uint32_t   offset     = (high_lane ? operand.op_sel_hi : operand.op_sel) ? 16u : 0u;
	if (operand.sdwa_sel == 4u || operand.sdwa_sel == 5u) {
		offset = operand.sdwa_sel == 5u ? 16u : 0u;
	}
	const auto half_u32 = IR::U32(
	    ir.Emit(IR::ValueOpcode::BitFieldUExtract, {bits, IR::Value(offset), IR::Value(16u)}));
	const auto half_u16 = IR::U16(ir.Emit(IR::ValueOpcode::ConvertU16U32, {half_u32}));
	const auto half     = IR::F16(ir.Emit(IR::ValueOpcode::BitCastF16U16, {half_u16}));
	auto       value    = IR::F32(ir.Emit(IR::ValueOpcode::ConvertF32F16, {half}));
	if (operand.absolute) {
		value = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {value}));
	}
	if (high_lane ? operand.negate_hi : operand.negate) {
		value = IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {value}));
	}
	return value;
}

IR::F32 Translator::ReadF16AsF32(const Decoder::Operand& operand) {
	return ReadF16LaneAsF32(operand, false);
}

IR::F32 Translator::ReadMixF32(const Decoder::Operand& operand) {
	if (operand.op_sel_hi) {
		return ReadF16AsF32(operand);
	}
	auto value_operand      = operand;
	value_operand.op_sel    = false;
	value_operand.op_sel_hi = false;
	value_operand.negate_hi = false;
	return IR::F32(ReadOperand(value_operand, IR::Type::F32));
}

IR::U32 Translator::ReadU16LaneRaw(const Decoder::Operand& operand, bool high_lane) {
	auto raw_operand      = operand;
	raw_operand.sdwa_sel  = 6;
	raw_operand.sdwa_sext = false;
	const auto bits       = ApplyBitSourceModifiers(raw_operand, ReadRawU32(operand));
	uint32_t   offset     = (high_lane ? operand.op_sel_hi : operand.op_sel) ? 16u : 0u;
	uint32_t   width      = 16u;
	if (operand.sdwa_sel <= 3u) {
		offset = operand.sdwa_sel * 8u;
		width  = 8u;
	} else if (operand.sdwa_sel == 4u || operand.sdwa_sel == 5u) {
		offset = operand.sdwa_sel == 5u ? 16u : 0u;
	}
	return IR::U32(
	    ir.Emit(IR::ValueOpcode::BitFieldUExtract, {bits, IR::Value(offset), IR::Value(width)}));
}

IR::U32 Translator::ReadU16LaneAsU32(const Decoder::Operand& operand, bool high_lane,
                                     bool sign_extend) {
	auto value = ReadU16LaneRaw(operand, high_lane);
	if (high_lane ? operand.negate_hi : operand.negate) {
		value = ir.BitwiseAnd(ir.ISub(IR::U32(IR::Value(0u)), value), IR::U32(IR::Value(0xffffu)));
	}
	if (sign_extend || operand.sdwa_sext) {
		value = IR::U32(
		    ir.Emit(IR::ValueOpcode::BitFieldSExtract, {value, IR::Value(0u), IR::Value(16u)}));
	}
	return value;
}

IR::U32 Translator::ReadU16AsU32(const Decoder::Operand& operand, bool sign_extend) {
	return ReadU16LaneAsU32(operand, false, sign_extend);
}

IR::U32 Translator::ReadF16LaneBits(const Decoder::Operand& operand, bool high_lane) {
	auto value = ReadU16LaneRaw(operand, high_lane);
	if (operand.absolute) {
		value = ir.BitwiseAnd(value, IR::U32(IR::Value(0x7fffu)));
	}
	if (high_lane ? operand.negate_hi : operand.negate) {
		value = ir.BitwiseXor(value, IR::U32(IR::Value(0x8000u)));
	}
	return value;
}

std::array<IR::U32, 2> Translator::ExtractU64(IR::U64 value) {
	return {ir.CompositeExtract(value, 0), ir.CompositeExtract(value, 1)};
}

void Translator::WriteU32Pair(const Decoder::Operand&       operand,
                              const std::array<IR::U32, 2>& value) {
	if (operand.kind == Decoder::OperandKind::Null) {
		return;
	}
	const auto thread_bit = ThreadBit(value[0]);
	switch (operand.kind) {
		case Decoder::OperandKind::ExecLo:
			ir.SetExec(thread_bit);
			ir.SetExecLo(value[0]);
			ir.SetExecHi(value[1]);
			return;
		case Decoder::OperandKind::VccLo:
			ir.SetVcc(thread_bit);
			ir.SetVccLo(value[0]);
			ir.SetVccHi(value[1]);
			return;
		case Decoder::OperandKind::Sgpr: break;
		default: break;
	}
	WriteRawU32(operand, value[0]);
	WriteRawU32(OffsetOperand(operand, 1), value[1]);
}

IR::U1 Translator::ThreadBit(IR::U32 low) {
	return ir.INotEqual(low, IR::U32(IR::Value(0u)));
}

IR::U1 Translator::ReadCondition(const Decoder::Operand& operand) {
	return IR::U1(ReadOperand(operand, IR::Type::U1));
}

IR::U32 Translator::ConditionBit(const Decoder::Operand& operand) {
	if (operand.kind == Decoder::OperandKind::LiteralConstant ||
	    operand.kind == Decoder::OperandKind::IntegerInlineConstant ||
	    operand.kind == Decoder::OperandKind::FloatInlineConstant) {
		return IR::U32(IR::Value(operand.value & 1u));
	}
	return ir.Select(ReadMask(operand), IR::U32(IR::Value(1u)), IR::U32(IR::Value(0u)));
}

IR::U1 Translator::ReadMask(const Decoder::Operand& operand) {
	if (operand.kind == Decoder::OperandKind::LiteralConstant ||
	    operand.kind == Decoder::OperandKind::IntegerInlineConstant ||
	    operand.kind == Decoder::OperandKind::FloatInlineConstant) {
		return IR::U1(IR::Value(operand.value != 0u));
	}
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: {
			const auto reg = static_cast<IR::ScalarReg>(operand.reg);
			return ir.GetThreadBitScalarReg(reg);
		}
		case Decoder::OperandKind::ExecLo:
		case Decoder::OperandKind::ExecHi: return ir.GetExec();
		case Decoder::OperandKind::VccLo:
		case Decoder::OperandKind::VccHi: return ir.GetVcc();
		case Decoder::OperandKind::Scc: return ir.GetScc();
		case Decoder::OperandKind::VccZ: return ir.LogicalNot(ir.GetVcc());
		case Decoder::OperandKind::ExecZ: return ir.LogicalNot(ir.GetExec());
		default: return ir.INotEqual(ReadRawU32(operand), IR::U32(IR::Value(0u)));
	}
}

IR::U1 Translator::ReadMaskValid(const Decoder::Operand& operand) {
	if (operand.kind == Decoder::OperandKind::LiteralConstant ||
	    operand.kind == Decoder::OperandKind::IntegerInlineConstant ||
	    operand.kind == Decoder::OperandKind::FloatInlineConstant) {
		const bool zero = operand.value == 0u;
		const bool ones = operand.value == 0xffffffffu &&
		                  operand.kind == Decoder::OperandKind::IntegerInlineConstant &&
		                  operand.signed_val < 0;
		return IR::U1(IR::Value(zero || ones));
	}
	switch (operand.kind) {
		case Decoder::OperandKind::Null:
		case Decoder::OperandKind::PopsExitingWaveId: return IR::U1(IR::Value(true));
		case Decoder::OperandKind::Sgpr:
			return ir.GetScalarMaskTag(static_cast<IR::ScalarReg>(operand.reg));
		case Decoder::OperandKind::ExecLo:
		case Decoder::OperandKind::ExecHi:
		case Decoder::OperandKind::VccLo:
		case Decoder::OperandKind::VccHi:
		case Decoder::OperandKind::VccZ:
		case Decoder::OperandKind::ExecZ:
		case Decoder::OperandKind::Scc: return IR::U1(IR::Value(true));
		default: return IR::U1(IR::Value(false));
	}
}

void Translator::WriteMask(const Decoder::Operand& operand, IR::U1 value) {
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: {
			const auto reg  = static_cast<IR::ScalarReg>(operand.reg);
			const auto mask = BallotMask(value);
			ir.SetThreadBitScalarReg(reg, value);
			ir.SetScalarMaskTag(reg, IR::U1(IR::Value(true)));
			if (IR::RegIndex(reg) > 0u) {
				ir.SetScalarMaskTag(static_cast<IR::ScalarReg>(IR::RegIndex(reg) - 1u),
				                    IR::U1(IR::Value(false)));
			}
			ir.SetScalarReg(reg, mask[0]);
			// A wave32 VALU mask destination must not overwrite the neighboring SGPR.
			if (current_wave_size == 64u && IR::RegIndex(reg) + 1u < IR::NumScalarRegs) {
				const auto high = static_cast<IR::ScalarReg>(IR::RegIndex(reg) + 1u);
				ir.SetScalarReg(high, mask[1]);
				ir.SetThreadBitScalarReg(high, IR::U1(IR::Value(false)));
				ir.SetScalarMaskTag(high, IR::U1(IR::Value(false)));
			}
			return;
		}
		case Decoder::OperandKind::ExecLo:
		case Decoder::OperandKind::ExecHi: {
			const auto mask = BallotMask(value);
			ir.SetExec(value);
			ir.SetExecLo(mask[0]);
			ir.SetExecHi(mask[1]);
			return;
		}
		case Decoder::OperandKind::VccLo:
		case Decoder::OperandKind::VccHi: {
			const auto mask = BallotMask(value);
			ir.SetVcc(value);
			ir.SetVccLo(mask[0]);
			ir.SetVccHi(mask[1]);
			return;
		}
		case Decoder::OperandKind::Scc: ir.SetScc(value); return;
		default:
			WriteRawU32(operand, ir.Select(value, IR::U32(IR::Value(1u)), IR::U32(IR::Value(0u))));
			WriteRawU32(OffsetOperand(operand, 1), IR::U32(IR::Value(0u)));
			return;
	}
}

void Translator::WriteMask64(const Decoder::Operand& operand, IR::U1 value) {
	if (operand.kind != Decoder::OperandKind::Sgpr) {
		WriteMask(operand, value);
		return;
	}
	const auto reg  = static_cast<IR::ScalarReg>(operand.reg);
	const auto mask = BallotMask(value);
	ir.SetThreadBitScalarReg(reg, value);
	ir.SetScalarMaskTag(reg, IR::U1(IR::Value(true)));
	if (IR::RegIndex(reg) > 0u) {
		ir.SetScalarMaskTag(static_cast<IR::ScalarReg>(IR::RegIndex(reg) - 1u),
		                    IR::U1(IR::Value(false)));
	}
	ir.SetScalarReg(reg, mask[0]);
	if (IR::RegIndex(reg) + 1u < IR::NumScalarRegs) {
		const auto high = static_cast<IR::ScalarReg>(IR::RegIndex(reg) + 1u);
		ir.SetScalarReg(high, mask[1]);
		ir.SetThreadBitScalarReg(high, IR::U1(IR::Value(false)));
		ir.SetScalarMaskTag(high, IR::U1(IR::Value(false)));
	}
}

void Translator::WriteCompareResult(const Decoder::Operand& operand, IR::U1 value) {
	if (operand.kind == Decoder::OperandKind::Scc) {
		WriteOperand(operand, value);
		return;
	}
	// VALU instructions only write lanes enabled by the incoming EXEC mask. This is
	// especially important for V_CMPX: replacing EXEC with the ungated comparison would
	// reactivate lanes disabled by an enclosing divergent region.
	WriteMask(operand, ir.LogicalAnd(ir.GetExec(), value));
}

bool Translator::AddBranchCondition(const CFG::BasicBlock& source, IR::BlockInfo& info,
                                    std::string* error) {
	if (source.terminator.goto_value >= 0) {
		if (source.terminator.goto_variable == UINT32_MAX) {
			return Fail(error, fmt::format("block {} sets an invalid goto variable", source.id));
		}
		ir.SetGotoVariable(source.terminator.goto_variable,
		                   IR::U1(IR::Value(source.terminator.goto_value != 0)));
	}
	if (source.terminator.kind == CFG::TerminatorKind::IndirectBranch) {
		if (source.terminator.indirect_selector_code != UINT32_MAX) {
			info.indirect_target = ReadScalarCode(source.terminator.indirect_selector_code);
		} else if (source.terminator.indirect_pc_sgpr != UINT32_MAX) {
			info.indirect_target =
			    ir.GetScalarReg(static_cast<IR::ScalarReg>(source.terminator.indirect_pc_sgpr));
		} else {
			return Fail(error, fmt::format("block {} has no indirect branch selector", source.id));
		}
		ir.Emit(IR::ValueOpcode::ReferenceU32, {info.indirect_target});
		return true;
	}
	if (source.terminator.kind != CFG::TerminatorKind::ConditionalBranch) {
		return true;
	}
	// EXEC and VCC are invocation-local Boolean masks. Branching on that Boolean lets inactive
	// invocations leave the region without reconstructing a host-subgroup mask.
	IR::U1 condition;
	switch (source.terminator.condition) {
		case CFG::BranchCondition::Always: condition = IR::U1(IR::Value(true)); break;
		case CFG::BranchCondition::SccZero: condition = ir.LogicalNot(ir.GetScc()); break;
		case CFG::BranchCondition::SccNonZero: condition = ir.GetScc(); break;
		case CFG::BranchCondition::VccZero: condition = ir.LogicalNot(ir.GetVcc()); break;
		case CFG::BranchCondition::VccNonZero: condition = ir.GetVcc(); break;
		case CFG::BranchCondition::ExecZero: condition = ir.LogicalNot(ir.GetExec()); break;
		case CFG::BranchCondition::ExecNonZero: condition = ir.GetExec(); break;
		case CFG::BranchCondition::GotoVariable:
			if (source.terminator.goto_variable == UINT32_MAX) {
				return Fail(error,
				            fmt::format("block {} reads an invalid goto variable", source.id));
			}
			condition = ir.GetGotoVariable(source.terminator.goto_variable);
			break;
		case CFG::BranchCondition::Unknown:
			return Fail(error, fmt::format("block {} has an unknown branch condition", source.id));
	}
	info.condition = condition;
	ir.Emit(IR::ValueOpcode::Reference, {condition});
	return true;
}

namespace {

bool IsCodeTableLoad(const CFG::Graph& cfg, uint32_t pc) {
	return std::ranges::find(cfg.code_table_load_pcs, pc) != cfg.code_table_load_pcs.end();
}

const EmbeddedFetchLoad* FindEmbeddedFetchLoad(const EmbeddedFetchPlan* plan, uint32_t pc) {
	if (plan == nullptr) {
		return nullptr;
	}
	const auto found = std::ranges::find(plan->loads, pc, &EmbeddedFetchLoad::pc);
	return found != plan->loads.end() ? &*found : nullptr;
}

bool IsEmbeddedFetchPrologLoad(const EmbeddedFetchPlan* plan, uint32_t pc) {
	if (plan == nullptr) {
		return false;
	}
	return std::ranges::any_of(plan->loads, [pc](const auto& load) {
		return std::ranges::find(load.prolog_loads, pc) != load.prolog_loads.end();
	});
}

int ResolveEmbeddedFetchResource(const ShaderVertexInputInfo& input,
                                 const EmbeddedFetchLoad&     load) {
	if (load.attrib_id >= 0 && load.attrib_id < input.resources_num &&
	    input.resources_dst[load.attrib_id].attr_id == load.attrib_id) {
		return load.attrib_id;
	}
	for (int index = 0; index < input.resources_num; index++) {
		const auto& destination = input.resources_dst[index];
		if (destination.attr_id == load.attrib_id &&
		    load.components <= static_cast<uint32_t>(std::max(destination.registers_num, 1))) {
			return index;
		}
	}
	for (int index = 0; index < input.resources_num; index++) {
		if (input.resources_dst[index].attr_id == load.attrib_id) {
			return index;
		}
	}
	return -1;
}

bool IsScalarMemoryLoad(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::S_LOAD_DWORD:
		case Decoder::Opcode::S_LOAD_DWORDX2:
		case Decoder::Opcode::S_LOAD_DWORDX4:
		case Decoder::Opcode::S_LOAD_DWORDX8:
		case Decoder::Opcode::S_LOAD_DWORDX16:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORD:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX2:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX4:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX8:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX16: return true;
		default: return false;
	}
}

bool IsBufferDwordLoad(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::BUFFER_LOAD_DWORD:
		case Decoder::Opcode::BUFFER_LOAD_DWORDX2:
		case Decoder::Opcode::BUFFER_LOAD_DWORDX3:
		case Decoder::Opcode::BUFFER_LOAD_DWORDX4:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_X:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XY:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XYZ:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XYZW:
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_X:
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_XY:
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_XYZ:
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_XYZW: return true;
		default: return false;
	}
}

void IncludeInstructionVectorRegisters(const Decoder::Instruction& inst, uint32_t& vector_limit) {
	const auto include_vector = [&](const Decoder::Operand& operand, uint32_t count = 1u) {
		if (operand.kind == Decoder::OperandKind::Vgpr) {
			vector_limit = std::min(IR::NumVectorRegs, std::max(vector_limit, operand.reg + count));
		}
	};
	const bool memory_family =
	    inst.family == Decoder::Family::MUBUF || inst.family == Decoder::Family::MTBUF ||
	    inst.family == Decoder::Family::FLAT || inst.family == Decoder::Family::DS ||
	    inst.family == Decoder::Family::MIMG;
	include_vector(inst.dst, memory_family ? std::max(inst.data_dwords, 1u) : 1u);
	include_vector(inst.dst2);
	include_vector(inst.src0);
	include_vector(inst.src1);
	include_vector(inst.src2);
	include_vector(inst.src3);
	if (inst.family == Decoder::Family::DS) {
		switch (inst.opcode) {
			case Decoder::Opcode::DS_WRITE_B64:
			case Decoder::Opcode::DS_WRITE_B96:
			case Decoder::Opcode::DS_WRITE_B128: include_vector(inst.src1, inst.data_dwords); break;
			case Decoder::Opcode::DS_WRITE2_B32:
			case Decoder::Opcode::DS_WRITE2ST64_B32:
			case Decoder::Opcode::DS_WRITE2_B64:
			case Decoder::Opcode::DS_WRITE2ST64_B64: {
				const auto width = std::max(inst.data_dwords / 2u, 1u);
				include_vector(inst.src1, width);
				include_vector(inst.src2, width);
				break;
			}
			default: break;
		}
	}
	for (uint32_t index = 0; index + 1u < inst.image_address_components &&
	                         index < Decoder::MaxImageNsaAddressComponents;
	     index++) {
		vector_limit =
		    std::min(IR::NumVectorRegs, std::max(vector_limit, inst.image_nsa_addr[index] + 1u));
	}
}

bool ValidateTranslateOptions(const TranslateOptions& options, std::string* error) {
	if (options.wave_size != 32u && options.wave_size != 64u) {
		return Fail(error, "shader translation requires wave32 or wave64");
	}
	switch (options.stage) {
		case ShaderType::Vertex:
			return options.vertex != nullptr ||
			       Fail(error, "vertex shader translation has no vertex input metadata");
		case ShaderType::Pixel:
			return options.pixel != nullptr ||
			       Fail(error, "pixel shader translation has no pixel input metadata");
		case ShaderType::Compute:
			return options.compute != nullptr ||
			       Fail(error, "compute shader translation has no compute input metadata");
		default: return Fail(error, "shader translation has an unsupported stage");
	}
}

} // namespace

bool TranslateProgram(const Decoder::Program& decoded, const CFG::Graph& cfg,
                      const TranslateOptions& options, IR::Program& result, std::string* error) {
	result = {};
	struct ResetOnFailure {
		IR::Program& program;
		bool         complete = false;

		~ResetOnFailure() {
			if (!complete) {
				program = {};
			}
		}
	} reset_on_failure {result};

	if (!ValidateTranslateOptions(options, error)) {
		return false;
	}
	if (cfg.blocks.empty()) {
		return Fail(error, "cannot translate an empty CFG");
	}

	result.stage               = options.stage;
	result.wave_size           = options.wave_size;
	result.shader_hash         = options.shader_hash;
	result.user_data_base      = options.user_data_base;
	result.user_data_count     = options.user_data_count;
	result.scratch_dwords      = options.scratch_dwords;
	result.dispatcher_fallback = options.dispatcher_fallback;
	result.cfg_failure_kind    = options.cfg_failure_kind;
	result.fallback_reason     = options.fallback_reason;
	if (options.embedded_fetch != nullptr) {
		result.info.vertex_offset_sgpr = options.embedded_fetch->vertex_offset_sgpr;
	}

	uint32_t vector_limit = 1u;
	for (const auto& cfg_block: cfg.blocks) {
		for (uint32_t index = cfg_block.inst_begin; index < cfg_block.inst_end; index++) {
			if (index >= decoded.instructions.size()) {
				return Fail(error,
				            "CFG block references an instruction outside the decoded program");
			}
			const auto& instruction = decoded.instructions[index];
			if (IsCodeTableLoad(cfg, instruction.pc)) {
				continue;
			}
			IncludeInstructionVectorRegisters(instruction, vector_limit);
		}
	}

	result.block_storage.reserve(cfg.blocks.size() + 1u);
	result.blocks.reserve(cfg.blocks.size() + 1u);
	result.block_info.reserve(cfg.blocks.size() + 1u);
	const auto max_id = std::ranges::max_element(cfg.blocks, {}, [](const CFG::BasicBlock& block) {
		                    return block.id;
	                    })->id;
	if (max_id == UINT32_MAX) {
		return Fail(error, "cannot allocate a typed entry block id");
	}
	CFG::Terminator terminator;
	terminator.kind       = CFG::TerminatorKind::Branch;
	terminator.true_block = cfg.blocks.front().id;
	result.block_storage.push_back(std::make_unique<IR::Block>());
	result.blocks.push_back(result.block_storage.back().get());
	result.block_info.push_back({max_id + 1u, cfg.blocks.front().start_pc,
	                             cfg.blocks.front().start_pc, std::move(terminator)});

	std::unordered_map<uint32_t, size_t> block_indices;
	block_indices.reserve(cfg.blocks.size());
	for (const auto& source_block: cfg.blocks) {
		if (!block_indices.emplace(source_block.id, result.blocks.size()).second) {
			return Fail(error, fmt::format("CFG contains duplicate block id {}", source_block.id));
		}
		result.block_storage.push_back(std::make_unique<IR::Block>());
		result.blocks.push_back(result.block_storage.back().get());
		result.block_info.push_back(
		    {source_block.id, source_block.start_pc, source_block.end_pc, source_block.terminator});
	}
	for (const auto& source_block: cfg.blocks) {
		const auto source_index = block_indices.at(source_block.id);
		for (const auto successor: source_block.successors) {
			const auto target = block_indices.find(successor);
			if (target == block_indices.end()) {
				return Fail(error, fmt::format("CFG block {} has unknown successor {}",
				                               source_block.id, successor));
			}
			result.blocks[source_index]->AddBranch(result.blocks[target->second]);
		}
	}
	{
		result.blocks.front()->AddBranch(result.blocks.at(block_indices.at(cfg.blocks.front().id)));
		IR::IREmitter entry_ir(result.blocks.front());
		const auto    builtin = [&](IR::StageInputKind kind, uint32_t component = 0u) {
			return IR::U32(
			    entry_ir.Emit(IR::ValueOpcode::GetBuiltin,
			                  {IR::Value(static_cast<uint32_t>(kind)), IR::Value(component)}));
		};
		for (uint32_t index = 0; index < options.user_data_count; index++) {
			const auto reg = static_cast<IR::ScalarReg>(options.user_data_base + index);
			if (IR::RegIndex(reg) >= IR::NumScalarRegs) {
				break;
			}
			const auto value = entry_ir.GetUserData(reg);
			entry_ir.SetScalarReg(reg, value);
			entry_ir.SetThreadBitScalarReg(reg, entry_ir.INotEqual(value, IR::U32(IR::Value(0u))));
			entry_ir.SetScalarMaskTag(reg, IR::U1(IR::Value(false)));
		}
		entry_ir.SetExec(IR::U1(IR::Value(true)));
		entry_ir.SetExecLo(IR::U32(IR::Value(1u)));
		entry_ir.SetExecHi(IR::U32(IR::Value(0u)));
		if (options.stage == ShaderType::Compute) {
			const auto* cs = options.compute;
			const auto  thread_ids =
			    cs->thread_ids_num > 0 ? std::min<uint32_t>(cs->thread_ids_num, 3u) : 0u;
			for (uint32_t index = 0; index < thread_ids; index++) {
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(index),
				                      builtin(IR::StageInputKind::LocalInvocationId, index));
			}
			uint32_t reg_offset = 0;
			for (uint32_t index = 0; index < 3u; index++) {
				if (cs->group_id[index]) {
					entry_ir.SetScalarReg(
					    static_cast<IR::ScalarReg>(cs->workgroup_register + reg_offset++),
					    builtin(IR::StageInputKind::WorkgroupId, index));
				}
			}
			if (cs->tg_size_en) {
				const auto wave_size     = cs->wave_size != 0u ? cs->wave_size : 64u;
				const auto total_threads = std::max(cs->threads_num[0], 1u) *
				                           std::max(cs->threads_num[1], 1u) *
				                           std::max(cs->threads_num[2], 1u);
				const auto waves = std::min((total_threads + wave_size - 1u) / wave_size, 0x3fu);
				const auto local_index = builtin(IR::StageInputKind::LocalInvocationIndex);
				const auto wave_id     = IR::U32(
				    entry_ir.Emit(IR::ValueOpcode::UDiv32, {local_index, IR::Value(wave_size)}));
				const auto wave_bits = entry_ir.ShiftLeftLogical(wave_id, IR::U32(IR::Value(20u)));
				const auto first_bit =
				    entry_ir.Select(entry_ir.IEqual(wave_id, IR::U32(IR::Value(0u))),
				                    IR::U32(IR::Value(0x80000000u)), IR::U32(IR::Value(0u)));
				entry_ir.SetScalarReg(
				    static_cast<IR::ScalarReg>(cs->workgroup_register + reg_offset),
				    entry_ir.BitwiseOr(entry_ir.BitwiseOr(wave_bits, IR::U32(IR::Value(waves))),
				                       first_bit));
			}
		} else if (options.stage == ShaderType::Pixel) {
			const auto* ps = options.pixel;
			if (ps->ps_perspective_center_vgpr != UINT32_MAX) {
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(ps->ps_perspective_center_vgpr),
				                      builtin(IR::StageInputKind::BaryCoordSmooth, 0));
				entry_ir.SetVectorReg(
				    static_cast<IR::VectorReg>(ps->ps_perspective_center_vgpr + 1u),
				    builtin(IR::StageInputKind::BaryCoordSmooth, 1));
			}
			uint32_t reg = ps->ps_system_input_base;
			if (ps->ps_pos_x) {
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(reg++),
				                      builtin(IR::StageInputKind::FragCoord, 0));
			}
			if (ps->ps_pos_y) {
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(reg++),
				                      builtin(IR::StageInputKind::FragCoord, 1));
			}
			if (ps->ps_pos_z) {
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(reg++),
				                      builtin(IR::StageInputKind::FragCoord, 2));
			}
			if (ps->ps_pos_w) {
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(reg++),
				                      builtin(IR::StageInputKind::FragCoord, 3));
			}
			if (ps->ps_front_face) {
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(reg),
				                      builtin(IR::StageInputKind::FrontFacing));
			}
		} else if (options.stage == ShaderType::Vertex) {
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(5),
			                      builtin(IR::StageInputKind::VertexIndex));
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(8),
			                      builtin(IR::StageInputKind::InstanceIndex));
		}
	}
	for (const auto& cfg_block: cfg.blocks) {
		const auto         typed_index = block_indices.at(cfg_block.id);
		Translator translator(result, result.blocks[typed_index], vector_limit, options.wave_size);
		for (uint32_t index = cfg_block.inst_begin; index < cfg_block.inst_end; index++) {
			const auto& instruction = decoded.instructions[index];
			if (IsCodeTableLoad(cfg, instruction.pc)) {
				continue;
			}
			if (IsScalarMemoryLoad(instruction.opcode) &&
			    IsEmbeddedFetchPrologLoad(options.embedded_fetch, instruction.pc)) {
				continue;
			}
			const auto* embedded = FindEmbeddedFetchLoad(options.embedded_fetch, instruction.pc);
			if (embedded != nullptr && IsBufferDwordLoad(instruction.opcode) &&
			    instruction.data_dwords == embedded->components &&
			    instruction.dst.kind == Decoder::OperandKind::Vgpr) {
				if (options.vertex == nullptr) {
					return Fail(error, "embedded vertex fetch plan has no vertex input metadata");
				}
				const auto resource = ResolveEmbeddedFetchResource(*options.vertex, *embedded);
				if (resource < 0 || resource >= options.vertex->resources_num) {
					return Fail(
					    error,
					    fmt::format(
					        "embedded vertex fetch at 0x{:08x} has no resource for attribute {}",
					        instruction.pc, embedded->attrib_id));
				}
				result.info.vertex_fetch_components[static_cast<size_t>(resource)] =
				    static_cast<uint8_t>(std::max<uint32_t>(
				        result.info.vertex_fetch_components[static_cast<size_t>(resource)],
				        embedded->components));
				if (!translator.TranslateEmbeddedFetch(instruction, static_cast<uint32_t>(resource),
				                                       embedded->components)) {
					return false;
				}
				continue;
			}
			if (!translator.TranslateInstruction(instruction, error)) {
				if (error == nullptr || error->empty()) {
					return Fail(error, fmt::format("opcode {} at 0x{:08x} has no IR translation",
					                               magic_enum::enum_name(instruction.opcode),
					                               instruction.pc));
				}
				return false;
			}
		}
		if (!translator.AddBranchCondition(cfg_block, result.block_info[typed_index], error)) {
			return false;
		}
	}
	if (!IR::ValidateProgram(result, false, error)) {
		return false;
	}
	reset_on_failure.complete = true;
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
