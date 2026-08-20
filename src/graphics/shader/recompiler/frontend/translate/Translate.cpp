#include "graphics/shader/recompiler/frontend/translate/Translator.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

using ShaderError::Fail;

namespace Detail {

IR::Operand Translator::OffsetOperand(const IR::Operand& operand, uint32_t offset) {
	if (offset == 0 || operand.kind != IR::OperandKind::Register) {
		return operand;
	}
	auto result = operand;
	result.reg.index += offset;
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

IR::Operand Translator::ScalarDestinationOperand(const IR::Operand& operand, uint32_t offset) {
	EXIT_IF(operand.kind != IR::OperandKind::Register);
	uint32_t code = 0;
	switch (operand.reg.file) {
		case IR::RegisterFile::Scalar: code = operand.reg.index; break;
		case IR::RegisterFile::Vcc: code = 106u + operand.reg.index; break;
		default: EXIT("invalid scalar-memory destination");
	}
	code += offset;
	IR::Operand result;
	result.kind = IR::OperandKind::Register;
	if (code < 106u) {
		result.reg = {IR::RegisterFile::Scalar, code};
	} else {
		switch (code) {
			case 106u: result.reg = {IR::RegisterFile::Vcc, 0u}; break;
			case 107u: result.reg = {IR::RegisterFile::Vcc, 1u}; break;
			default: EXIT("scalar-memory destination crosses an invalid register");
		}
	}
	return result;
}

IR::Operand Translator::PlainOperand(const IR::Operand& operand) {
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

IR::U32 Translator::ReadPcRelativeU32(const IR::Operand& operand) {
	EXIT_IF(operand.kind != IR::OperandKind::PcRelativeU32 &&
	        operand.kind != IR::OperandKind::PcRelativeHighU32);
	const auto address =
	    ir.Emit(IR::ValueOpcode::IAdd64, {ir.Emit(IR::ValueOpcode::GetShaderBase),
	                                      IR::Value(static_cast<uint64_t>(operand.imm))});
	return ir.CompositeExtract(address, operand.kind == IR::OperandKind::PcRelativeU32 ? 0u : 1u);
}

std::array<IR::U32, 2> Translator::BallotMask(IR::U1 value) {
	if (current_per_invocation_masks) {
		return {ir.Select(value, IR::U32(IR::Value(1u)), IR::U32(IR::Value(0u))),
		        IR::U32(IR::Value(0u))};
	}
	const auto ballot = ir.Emit(IR::ValueOpcode::Ballot, {value});
	const auto low =
	    IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU32x4, {ballot, IR::Value(0u)}));
	// RDNA2 wave32 masks have one architectural dword; the upper half is unused.
	const auto high =
	    current_wave_size == 32u
	        ? IR::U32(IR::Value(0u))
	        : IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU32x4, {ballot, IR::Value(1u)}));
	return {low, high};
}

IR::U32 Translator::ReadRawU32(const IR::Operand& operand) {
	if (operand.kind == IR::OperandKind::ImmediateU32) {
		return IR::U32(IR::Value(operand.imm));
	}
	if (operand.kind == IR::OperandKind::PcRelativeU32 ||
	    operand.kind == IR::OperandKind::PcRelativeHighU32) {
		return ReadPcRelativeU32(operand);
	}
	if (operand.kind == IR::OperandKind::Null) {
		return IR::U32(IR::Value(0u));
	}
	EXIT_IF(operand.kind != IR::OperandKind::Register);
	switch (operand.reg.file) {
		case IR::RegisterFile::Scalar:
			return ir.GetScalarReg(static_cast<IR::ScalarReg>(operand.reg.index));
		case IR::RegisterFile::Vector:
			return ir.GetVectorReg(static_cast<IR::VectorReg>(operand.reg.index));
		case IR::RegisterFile::Vcc: return operand.reg.index == 0 ? ir.GetVccLo() : ir.GetVccHi();
		case IR::RegisterFile::M0: return ir.GetM0();
		case IR::RegisterFile::Exec:
			return operand.reg.index == 0 ? ir.GetExecLo() : ir.GetExecHi();
		case IR::RegisterFile::Scc:
			return ir.Select(ir.GetScc(), IR::U32(IR::Value(1u)), IR::U32(IR::Value(0u)));
	}
	EXIT("invalid register file used as a raw U32 source");
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

IR::U32 Translator::ApplyBitSourceModifiers(const IR::Operand& operand, IR::U32 value) {
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

IR::Value Translator::ReadOperand(const IR::Operand& operand, IR::Type type) {
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
		EXIT_IF(operand.kind != IR::OperandKind::Register);
		switch (operand.reg.file) {
			case IR::RegisterFile::Scc: return ir.GetScc();
			case IR::RegisterFile::Exec: return ir.GetExec();
			case IR::RegisterFile::Vcc: return ir.GetVcc();
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
		EXIT("opcode %s at 0x%08x requested unsupported operand type %s",
		     IR::OpcodeName(current_opcode).data(), current_pc, IR::TypeName(type).c_str());
	}
	return bits;
}

void Translator::WriteRawU32(const IR::Operand& operand, IR::U32 value) {
	if (operand.kind == IR::OperandKind::Null) {
		return;
	}
	EXIT_IF(operand.kind != IR::OperandKind::Register);
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
	switch (operand.reg.file) {
		case IR::RegisterFile::Scalar:
			ir.SetScalarReg(static_cast<IR::ScalarReg>(operand.reg.index), value);
			break;
		case IR::RegisterFile::Vector: {
			const auto reg = static_cast<IR::VectorReg>(operand.reg.index);
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
		case IR::RegisterFile::Vcc:
			if (operand.reg.index == 0) {
				ir.SetVccLo(IR::U32(value));
				ir.SetVcc(ThreadBit(IR::U32(value), ir.GetVccHi()));
			} else {
				ir.SetVccHi(IR::U32(value));
				ir.SetVcc(ThreadBit(ir.GetVccLo(), IR::U32(value)));
			}
			break;
		case IR::RegisterFile::M0: ir.SetM0(IR::U32(value)); break;
		case IR::RegisterFile::Exec:
			if (operand.reg.index == 0) {
				ir.SetExecLo(IR::U32(value));
				ir.SetExec(ThreadBit(IR::U32(value), ir.GetExecHi()));
			} else {
				ir.SetExecHi(IR::U32(value));
				ir.SetExec(ThreadBit(ir.GetExecLo(), IR::U32(value)));
			}
			break;
		case IR::RegisterFile::Scc: ir.SetScc(ir.INotEqual(value, IR::U32(IR::Value(0u)))); break;
	}
}

IR::F32 Translator::ApplyF32ResultModifiers(const IR::Operand& operand, IR::F32 value) {
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

void Translator::WriteOperand(const IR::Operand& operand, IR::Value value) {
	if (operand.kind == IR::OperandKind::Null) {
		return;
	}
	auto type = value.GetType();
	if (type == IR::Type::F32) {
		value = ApplyF32ResultModifiers(operand, IR::F32(value));
		type  = IR::Type::F32;
	}
	if (type == IR::Type::Opaque) {
		EXIT("opcode %s at 0x%08x produced an untyped value", IR::OpcodeName(current_opcode).data(),
		     current_pc);
	}
	if (type == IR::Type::U1) {
		EXIT_IF(operand.kind != IR::OperandKind::Register);
		switch (operand.reg.file) {
			case IR::RegisterFile::Scc: ir.SetScc(IR::U1(value)); return;
			case IR::RegisterFile::Exec: {
				const auto mask = BallotMask(IR::U1(value));
				ir.SetExec(IR::U1(value));
				ir.SetExecLo(mask[0]);
				ir.SetExecHi(mask[1]);
				return;
			}
			case IR::RegisterFile::Vcc: {
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

void Translator::WriteF16(const IR::Operand& operand, IR::F32 value) {
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

void Translator::WriteU16(const IR::Operand& operand, IR::U32 value) {
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

IR::U32 Translator::ReadU32(const IR::Operand& operand) {
	return IR::U32(ReadOperand(operand, IR::Type::U32));
}

std::array<IR::U32, 2> Translator::ReadU32Pair(const IR::Operand& operand) {
	if (operand.kind == IR::OperandKind::Register) {
		if (operand.reg.file == IR::RegisterFile::Exec) {
			return {ir.GetExecLo(), ir.GetExecHi()};
		}
		if (operand.reg.file == IR::RegisterFile::Vcc) {
			return {ir.GetVccLo(), ir.GetVccHi()};
		}
	}
	const auto low = ApplyBitSourceModifiers(operand, ReadRawU32(operand));
	IR::U32    high(IR::Value(0u));
	if (operand.kind == IR::OperandKind::Register) {
		high = ReadRawU32(OffsetOperand(operand, 1));
	} else if (operand.kind == IR::OperandKind::ImmediateU32 && operand.sext_64) {
		high = IR::U32(IR::Value(0xffffffffu));
	}
	return {low, high};
}

IR::U64 Translator::ReadU64(const IR::Operand& operand) {
	return IR::U64(ReadOperand(operand, IR::Type::U64));
}

IR::F32 Translator::ReadF16LaneAsF32(const IR::Operand& operand, bool high_lane, bool packed) {
	if (operand.float_inline) {
		const bool use_zero = packed && (high_lane ? operand.op_sel_hi : operand.op_sel);
		auto value = use_zero ? IR::F32(IR::Value::F32(0.0f))
		                      : ir.BitCastF32(IR::U32(IR::Value(operand.imm)));
		const auto half = IR::F16(ir.Emit(IR::ValueOpcode::ConvertF16F32, {value}));
		value           = IR::F32(ir.Emit(IR::ValueOpcode::ConvertF32F16, {half}));
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

IR::F32 Translator::ReadF16AsF32(const IR::Operand& operand) {
	return ReadF16LaneAsF32(operand, false);
}

IR::F32 Translator::ReadMixF32(const IR::Operand& operand) {
	if (operand.op_sel_hi) {
		return ReadF16AsF32(operand);
	}
	auto value_operand      = operand;
	value_operand.op_sel    = false;
	value_operand.op_sel_hi = false;
	value_operand.negate_hi = false;
	return IR::F32(ReadOperand(value_operand, IR::Type::F32));
}

IR::U32 Translator::ReadU16LaneRaw(const IR::Operand& operand, bool high_lane) {
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

IR::U32 Translator::ReadU16LaneAsU32(const IR::Operand& operand, bool high_lane, bool sign_extend) {
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

IR::U32 Translator::ReadU16AsU32(const IR::Operand& operand, bool sign_extend) {
	return ReadU16LaneAsU32(operand, false, sign_extend);
}

IR::U32 Translator::ReadF16LaneBits(const IR::Operand& operand, bool high_lane) {
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

void Translator::WriteU32Pair(const IR::Operand& operand, const std::array<IR::U32, 2>& value) {
	if (operand.kind == IR::OperandKind::Null) {
		return;
	}
	if (operand.kind == IR::OperandKind::Register) {
		const auto thread_bit = ThreadBit(value[0], value[1]);
		switch (operand.reg.file) {
			case IR::RegisterFile::Exec:
				ir.SetExec(thread_bit);
				ir.SetExecLo(value[0]);
				ir.SetExecHi(value[1]);
				return;
			case IR::RegisterFile::Vcc:
				ir.SetVcc(thread_bit);
				ir.SetVccLo(value[0]);
				ir.SetVccHi(value[1]);
				return;
			case IR::RegisterFile::Scalar:
				ir.SetThreadBitScalarReg(static_cast<IR::ScalarReg>(operand.reg.index), thread_bit);
				break;
			default: break;
		}
	}
	WriteRawU32(operand, value[0]);
	WriteRawU32(OffsetOperand(operand, 1), value[1]);
}

IR::U1 Translator::ThreadBit(IR::U32 low, IR::U32 high) {
	if (current_per_invocation_masks) {
		return ir.INotEqual(low, IR::U32(IR::Value(0u)));
	}
	const auto lane = IR::U32(ir.Emit(IR::ValueOpcode::LaneId));
	auto       word = low;
	if (current_wave_size == 64u) {
		const auto high_lane =
		    IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThanEqual32, {lane, IR::Value(32u)}));
		word = ir.Select(high_lane, high, low);
	}
	const auto bit =
	    ir.BitwiseAnd(ir.ShiftRightLogical(word, ir.BitwiseAnd(lane, IR::U32(IR::Value(31u)))),
	                  IR::U32(IR::Value(1u)));
	return ir.INotEqual(bit, IR::U32(IR::Value(0u)));
}

IR::U1 Translator::ReadCondition(const IR::Operand& operand) {
	return IR::U1(ReadOperand(operand, IR::Type::U1));
}

IR::U32 Translator::ConditionBit(const IR::Operand& operand) {
	if (operand.kind == IR::OperandKind::ImmediateU32) {
		return IR::U32(IR::Value(operand.imm & 1u));
	}
	return ir.Select(ReadMask(operand), IR::U32(IR::Value(1u)), IR::U32(IR::Value(0u)));
}

IR::U1 Translator::ReadMask(const IR::Operand& operand) {
	if (operand.kind == IR::OperandKind::ImmediateU32) {
		return IR::U1(IR::Value(operand.imm != 0u || operand.sext_64));
	}
	EXIT_IF(operand.kind != IR::OperandKind::Register);
	switch (operand.reg.file) {
		case IR::RegisterFile::Scalar: {
			const auto reg = static_cast<IR::ScalarReg>(operand.reg.index);
			const auto high =
			    current_wave_size == 64u && IR::RegIndex(reg) + 1u < IR::NumScalarRegs
			        ? ir.GetScalarReg(static_cast<IR::ScalarReg>(IR::RegIndex(reg) + 1u))
			        : IR::U32(IR::Value(0u));
			return ThreadBit(ir.GetScalarReg(reg), high);
		}
		case IR::RegisterFile::Exec: return ir.GetExec();
		case IR::RegisterFile::Vcc: return ir.GetVcc();
		case IR::RegisterFile::Scc: return ir.GetScc();
		default: return ir.INotEqual(ReadRawU32(operand), IR::U32(IR::Value(0u)));
	}
}

void Translator::WriteMask(const IR::Operand& operand, IR::U1 value) {
	EXIT_IF(operand.kind != IR::OperandKind::Register);
	switch (operand.reg.file) {
		case IR::RegisterFile::Scalar: {
			const auto reg  = static_cast<IR::ScalarReg>(operand.reg.index);
			const auto mask = BallotMask(value);
			ir.SetThreadBitScalarReg(reg, value);
			ir.SetScalarReg(reg, mask[0]);
			// A wave32 VALU mask destination must not overwrite the neighboring SGPR.
			if (current_wave_size == 64u && IR::RegIndex(reg) + 1u < IR::NumScalarRegs) {
				ir.SetScalarReg(static_cast<IR::ScalarReg>(IR::RegIndex(reg) + 1u), mask[1]);
			}
			return;
		}
		case IR::RegisterFile::Exec: {
			const auto mask = BallotMask(value);
			ir.SetExec(value);
			ir.SetExecLo(mask[0]);
			ir.SetExecHi(mask[1]);
			return;
		}
		case IR::RegisterFile::Vcc: {
			const auto mask = BallotMask(value);
			ir.SetVcc(value);
			ir.SetVccLo(mask[0]);
			ir.SetVccHi(mask[1]);
			return;
		}
		case IR::RegisterFile::Scc: ir.SetScc(value); return;
		default:
			WriteRawU32(operand, ir.Select(value, IR::U32(IR::Value(1u)), IR::U32(IR::Value(0u))));
			WriteRawU32(OffsetOperand(operand, 1), IR::U32(IR::Value(0u)));
			return;
	}
}

void Translator::WriteMask64(const IR::Operand& operand, IR::U1 value) {
	if (operand.kind != IR::OperandKind::Register || operand.reg.file != IR::RegisterFile::Scalar) {
		WriteMask(operand, value);
		return;
	}
	const auto reg  = static_cast<IR::ScalarReg>(operand.reg.index);
	const auto mask = BallotMask(value);
	ir.SetThreadBitScalarReg(reg, value);
	ir.SetScalarReg(reg, mask[0]);
	if (IR::RegIndex(reg) + 1u < IR::NumScalarRegs) {
		ir.SetScalarReg(static_cast<IR::ScalarReg>(IR::RegIndex(reg) + 1u), mask[1]);
	}
}

void Translator::WriteCompareResult(const IR::Operand& operand, IR::U1 value) {
	if (operand.kind == IR::OperandKind::Register && operand.reg.file == IR::RegisterFile::Scc) {
		WriteOperand(operand, value);
		return;
	}
	// VALU instructions only write lanes enabled by the incoming EXEC mask. This is
	// especially important for V_CMPX: replacing EXEC with the ungated comparison would
	// reactivate lanes disabled by an enclosing divergent region.
	WriteMask(operand, ir.LogicalAnd(ir.GetExec(), value));
}

bool Translator::TranslateBlock(const IR::BasicBlock& source, std::string* error) {
	for (const auto& source_inst: source.instructions) {
		current_opcode = source_inst.op;
		current_pc     = source_inst.pc;
		if (static_cast<size_t>(source_inst.op) >= static_cast<size_t>(IR::Opcode::Count)) {
			return Fail(error, "value IR input opcode is out of range");
		}
		if (TranslateInstruction(source_inst)) {
			continue;
		}
		return Fail(error, fmt::format("opcode {} at 0x{:08x} has no typed Value IR lowering",
		                               IR::OpcodeName(source_inst.op), source_inst.pc));
	}
	return true;
}

bool Translator::AddBranchCondition(const IR::BasicBlock& source, IR::ValueBlockInfo& info,
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
	IR::U1     condition;
	const auto mask_non_zero = [&](IR::U1 value) {
		const auto mask = BallotMask(value);
		return ir.INotEqual(ir.BitwiseOr(mask[0], mask[1]), IR::U32(IR::Value(0u)));
	};
	switch (source.terminator.condition) {
		case CFG::BranchCondition::Always: condition = IR::U1(IR::Value(true)); break;
		case CFG::BranchCondition::SccZero: condition = ir.LogicalNot(ir.GetScc()); break;
		case CFG::BranchCondition::SccNonZero: condition = ir.GetScc(); break;
		case CFG::BranchCondition::VccZero:
			condition = ir.LogicalNot(mask_non_zero(ir.GetVcc()));
			break;
		case CFG::BranchCondition::VccNonZero: condition = mask_non_zero(ir.GetVcc()); break;
		case CFG::BranchCondition::ExecZero:
			condition = ir.LogicalNot(mask_non_zero(ir.GetExec()));
			break;
		case CFG::BranchCondition::ExecNonZero: condition = mask_non_zero(ir.GetExec()); break;
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

} // namespace Detail

bool TranslateProgram(const IR::Program& source, IR::ValueProgram& result,
                      const ShaderVertexInputInfo*  vertex_input_info,
                      const ShaderPixelInputInfo*   pixel_input_info,
                      const ShaderComputeInputInfo* compute_input_info, std::string* error) {
	result                          = {};
	const uint32_t wave_size        = source.wave_size;
	const bool per_invocation_masks = source.lane_mask_mode == ShaderLaneMaskMode::PerInvocation;
	uint32_t   vector_limit         = 1u;
	const auto include_vector       = [&](const IR::Operand& operand, uint32_t count = 1u) {
		if (operand.kind == IR::OperandKind::Register &&
		    operand.reg.file == IR::RegisterFile::Vector) {
			vector_limit =
			    std::min(IR::NumVectorRegs, std::max(vector_limit, operand.reg.index + count));
		}
	};
	for (const auto& block: source.blocks) {
		for (const auto& inst: block.instructions) {
			include_vector(inst.dst);
			include_vector(inst.dst2);
			for (uint32_t index = 0; index < inst.src_count; index++) {
				include_vector(inst.src[index]);
			}
			if (inst.op == IR::Opcode::BufferLoadDword) {
				include_vector(inst.dst, std::max(inst.memory.data_dwords, 1u));
			} else if (inst.op == IR::Opcode::BufferStoreDword) {
				include_vector(inst.src[0], std::max(inst.memory.data_dwords, 1u));
			} else if (inst.op == IR::Opcode::DsReadB32 || inst.op == IR::Opcode::DsRead2B32) {
				include_vector(inst.dst, std::max(inst.memory.data_dwords, 1u));
			} else if (inst.op == IR::Opcode::DsWriteB32) {
				include_vector(inst.src[0], std::max(inst.memory.data_dwords, 1u));
			} else if (inst.op == IR::Opcode::DsWrite2B32) {
				const auto width = std::max(inst.memory.data_dwords / 2u, 1u);
				include_vector(inst.src[0], width);
				include_vector(inst.src[2], width);
			} else if (inst.op == IR::Opcode::LoadInputF32) {
				include_vector(inst.dst, std::max(inst.input_info.component_count, 1u));
			}
		}
	}
	result.block_storage.reserve(source.blocks.size() + 1u);
	result.blocks.reserve(source.blocks.size() + 1u);
	result.block_info.reserve(source.blocks.size() + 1u);
	if (!source.blocks.empty()) {
		const auto max_id =
		    std::ranges::max_element(source.blocks, {}, [](const IR::BasicBlock& block) {
			    return block.id;
		    })->id;
		if (max_id == UINT32_MAX) {
			return Fail(error, "cannot allocate a typed entry block id");
		}
		CFG::Terminator terminator;
		terminator.kind       = CFG::TerminatorKind::Branch;
		terminator.true_block = source.blocks.front().id;
		result.block_storage.push_back(std::make_unique<IR::Block>());
		result.blocks.push_back(result.block_storage.back().get());
		result.block_info.push_back({max_id + 1u, source.blocks.front().start_pc,
		                             source.blocks.front().start_pc, std::move(terminator)});
	}
	for (const auto& source_block: source.blocks) {
		result.block_storage.push_back(std::make_unique<IR::Block>());
		result.blocks.push_back(result.block_storage.back().get());
		result.block_info.push_back(
		    {source_block.id, source_block.start_pc, source_block.end_pc, source_block.terminator});
	}
	for (size_t index = 0; index < source.blocks.size(); index++) {
		for (const auto successor: source.blocks[index].successors) {
			if (successor >= source.blocks.size()) {
				return Fail(error, "value IR block successor is out of range");
			}
			result.blocks[index + 1u]->AddBranch(result.blocks[successor + 1u]);
		}
	}
	if (!result.blocks.empty()) {
		result.blocks.front()->AddBranch(result.blocks[1]);
		IR::IREmitter entry_ir(result.blocks.front());
		const auto    builtin = [&](IR::StageInputKind kind, uint32_t component = 0u) {
			return IR::U32(
			    entry_ir.Emit(IR::ValueOpcode::GetBuiltin,
			                  {IR::Value(static_cast<uint32_t>(kind)), IR::Value(component)}));
		};
		for (uint32_t index = 0; index < source.user_data_count; index++) {
			const auto reg = static_cast<IR::ScalarReg>(source.user_data_base + index);
			if (IR::RegIndex(reg) >= IR::NumScalarRegs) {
				break;
			}
			entry_ir.SetScalarReg(reg, entry_ir.GetUserData(reg));
		}
		entry_ir.SetExec(IR::U1(IR::Value(true)));
		if (source.stage == ShaderType::Compute && !per_invocation_masks) {
			const auto active = entry_ir.Emit(IR::ValueOpcode::Ballot, {IR::Value(true)});
			entry_ir.SetExecLo(entry_ir.CompositeExtract(active, 0));
			entry_ir.SetExecHi(source.wave_size > 32u ? entry_ir.CompositeExtract(active, 1)
			                                               : IR::U32(IR::Value(0u)));
		} else {
			entry_ir.SetExecLo(IR::U32(IR::Value(per_invocation_masks ? 1u : 0xffffffffu)));
			entry_ir.SetExecHi(IR::U32(
			    IR::Value(!per_invocation_masks && source.wave_size > 32u ? 0xffffffffu : 0u)));
		}
		if (source.stage == ShaderType::Compute) {
			const auto* cs = compute_input_info;
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
		} else if (source.stage == ShaderType::Pixel) {
			const auto* ps  = pixel_input_info;
			uint32_t    reg = ps->ps_system_input_base;
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
		} else if (source.stage == ShaderType::Vertex) {
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(5),
			                      builtin(IR::StageInputKind::VertexIndex));
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(8),
			                      builtin(IR::StageInputKind::InstanceIndex));
		}
	}
	for (size_t index = 0; index < source.blocks.size(); index++) {
		Detail::Translator translator(result, result.blocks[index + 1u], vector_limit, wave_size,
		                              per_invocation_masks);
		if (!translator.TranslateBlock(source.blocks[index], error) ||
		    !translator.AddBranchCondition(source.blocks[index], result.block_info[index + 1u],
		                                   error)) {
			return false;
		}
	}
	return IR::ValidateValueProgram(result, false, error);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
