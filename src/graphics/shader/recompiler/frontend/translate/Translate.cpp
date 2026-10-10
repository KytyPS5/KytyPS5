#include "common/assert.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <unordered_map>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

static IR::DppMoveFlags DppFlags(const Decoder::Operand& operand) {
	return {
	    .control        = operand.dpp_ctrl,
	    .row_mask       = operand.dpp_row_mask,
	    .bank_mask      = operand.dpp_bank_mask,
	    .fetch_inactive = operand.dpp_fetch_inactive,
	    .bound_control  = operand.dpp_bound_ctrl,
	    .dpp8           = operand.dpp8,
	};
}

const Decoder::Operand& Translator::SourceAt(const Decoder::Instruction& inst, uint32_t index) {
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
		const auto& source = SourceAt(inst, index);
		if (!source.dpp) {
			continue;
		}
		destination.dpp                = true;
		destination.dpp8               = source.dpp8;
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
	result.explicit_sdwa_dst  = false;
	result.dpp_fetch_inactive = false;
	result.dpp_bound_ctrl     = false;
	result.dpp                = false;
	result.dpp8               = false;
	return result;
}

std::array<IR::U32, 2> Translator::BallotMask(IR::U1 value) {
	const auto mask = ir.Emit(IR::ValueOpcode::Ballot, {value});
	return {ir.CompositeExtract(mask, 0),
	        program.wave_size == 64u ? ir.CompositeExtract(mask, 1) : IR::U32(IR::Value(0u))};
}

IR::U32 Translator::ReadRawU32(const Decoder::Operand& operand) {
	switch (operand.kind) {
		case Decoder::OperandKind::LiteralConstant:
		case Decoder::OperandKind::IntegerInlineConstant:
		case Decoder::OperandKind::FloatInlineConstant: return IR::U32(IR::Value(operand.value));
		case Decoder::OperandKind::Null:
		case Decoder::OperandKind::SharedBase:
		case Decoder::OperandKind::PrivateBase:
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
		case Decoder::OperandKind::ExecZ: {
			const bool vcc = operand.kind == Decoder::OperandKind::VccZ;
			auto mask = vcc ? ir.GetVccLo() : ir.GetExecLo();
			if (program.wave_size == 64u) {
				mask = ir.BitwiseOr(mask, vcc ? ir.GetVccHi() : ir.GetExecHi());
			}
			const auto zero = IR::U32(IR::Value(0u));
			return ir.Select(ir.IEqual(mask, zero), IR::U32(IR::Value(1u)), zero);
		}
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
		value =
		    IR::U32(ir.Emit(IR::ValueOpcode::DppMoveU32, {value, ir.GetExec()}, DppFlags(operand)));
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
	if (type == IR::Type::U64 || type == IR::Type::F64) {
		auto pair = ReadU32Pair(operand);
		if (type == IR::Type::F64) {
			if (operand.kind == Decoder::OperandKind::LiteralConstant) {
				pair = {IR::U32(IR::Value(0u)), IR::U32(IR::Value(operand.value))};
			} else if (operand.kind == Decoder::OperandKind::FloatInlineConstant) {
				const auto bits = operand.value == 0x3e22f983u
				                      ? 0x3fc45f306dc9c882ull
				                      : std::bit_cast<uint64_t>(static_cast<double>(
				                            std::bit_cast<float>(operand.value)));
				pair            = {IR::U32(IR::Value(static_cast<uint32_t>(bits))),
				                   IR::U32(IR::Value(static_cast<uint32_t>(bits >> 32u)))};
			}
			if (operand.absolute) {
				pair[1] = ir.BitwiseAnd(pair[1], IR::U32(IR::Value(0x7fffffffu)));
			}
			if (operand.negate) {
				pair[1] = ir.BitwiseXor(pair[1], IR::U32(IR::Value(0x80000000u)));
			}
		}
		const auto bits = ir.ConstructU64(pair[0], pair[1]);
		return type == IR::Type::F64 ? ir.Emit(IR::ValueOpcode::BitCastF64U64, {bits}) : IR::Value(bits);
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
			program.scalar_writes.push_back({current_pc, reg});
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
				value = IR::U32(ir.Emit(IR::ValueOpcode::DppUpdateU32, {value, old, ir.GetExec()},
				                        DppFlags(operand)));
			} else {
				value = ir.Select(ir.GetExec(), value, old);
			}
			ir.SetVectorReg(reg, value);
			break;
		}
		case Decoder::OperandKind::VccLo:
			ir.SetVccLo(IR::U32(value));
			ir.SetVcc(ThreadBit({value, ir.GetVccHi()}));
			break;
		case Decoder::OperandKind::VccHi:
			ir.SetVccHi(IR::U32(value));
			ir.SetVcc(ThreadBit({ir.GetVccLo(), value}));
			break;
		case Decoder::OperandKind::M0: ir.SetM0(IR::U32(value)); break;
		case Decoder::OperandKind::ExecLo:
			ir.SetExecLo(IR::U32(value));
			ir.SetExec(ThreadBit({value, ir.GetExecHi()}));
			break;
		case Decoder::OperandKind::ExecHi:
			ir.SetExecHi(IR::U32(value));
			ir.SetExec(ThreadBit({ir.GetExecLo(), value}));
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
		Write16Bits(operand, IR::U32(ir.Emit(IR::ValueOpcode::ConvertU32U16, {IR::U16(value)})));
		return;
	}
	if (type == IR::Type::F16) {
		const auto bits = IR::U16(ir.Emit(IR::ValueOpcode::BitCastU16F16, {value}));
		Write16Bits(operand, IR::U32(ir.Emit(IR::ValueOpcode::ConvertU32U16, {bits})));
		return;
	}
	if (type == IR::Type::F64) {
		value = ir.Emit(IR::ValueOpcode::BitCastU64F64, {value});
		type  = IR::Type::U64;
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

void Translator::Write16Bits(const Decoder::Operand& operand, IR::U32 value) {
	auto destination = operand;
	// Native GFX10 16-bit results preserve the unselected half. Plain/DPP destinations use the
	// low half; native VOP3 may already select either half. Explicit SDWA retains encoded DST_U.
	if (!destination.explicit_sdwa_dst) {
		if (destination.sdwa_sel == 6u) {
			destination.sdwa_sel = 4u;
		}
		destination.sdwa_dst_unused = 2u;
	}
	destination.omod   = 0u;
	destination.op_sel = false;
	destination.clamp  = false;
	WriteRawU32(destination, ir.BitwiseAnd(value, IR::U32(IR::Value(0xffffu))));
}

void Translator::WriteF16(const Decoder::Operand& operand, IR::F32 value) {
	value           = ApplyF32ResultModifiers(operand, value);
	const auto half = IR::F16(ir.Emit(IR::ValueOpcode::ConvertF16F32, {value}));
	const auto bits = IR::U32(ir.Emit(IR::ValueOpcode::ConvertU32U16,
	                                  {IR::U16(ir.Emit(IR::ValueOpcode::BitCastU16F16, {half}))}));
	Write16Bits(operand, bits);
}

IR::U32 Translator::ReadU32(const Decoder::Operand& operand) {
	return IR::U32(ReadOperand(operand, IR::Type::U32));
}

std::array<IR::U32, 2> Translator::ReadU32Pair(const Decoder::Operand& operand) {
	if (operand.kind == Decoder::OperandKind::PrivateBase ||
	    operand.kind == Decoder::OperandKind::SharedBase) {
		return {IR::U32(IR::Value(0u)), IR::U32(IR::Value(
		    operand.kind == Decoder::OperandKind::PrivateBase ? Decoder::PrivateApertureHigh
		                                                      : Decoder::SharedApertureHigh))};
	}
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

IR::F32 Translator::ReadF16LaneAsF32(const Decoder::Operand& operand, bool high_lane) {
	const auto bits     = Read16LaneBits(operand, high_lane);
	const auto half_u16 = IR::U16(ir.Emit(IR::ValueOpcode::ConvertU16U32, {bits}));
	const auto half     = IR::F16(ir.Emit(IR::ValueOpcode::BitCastF16U16, {half_u16}));
	return IR::F32(ir.Emit(IR::ValueOpcode::ConvertF32F16, {half}));
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

IR::U32 Translator::ReadU16LaneAsU32(const Decoder::Operand& operand, bool high_lane,
                                     bool sign_extend) {
	auto value = Read16LaneBits(operand, high_lane);
	if (sign_extend) {
		value = IR::U32(
		    ir.Emit(IR::ValueOpcode::BitFieldSExtract, {value, IR::Value(0u), IR::Value(16u)}));
	}
	return value;
}

IR::U32 Translator::ReadU16AsU32(const Decoder::Operand& operand, bool sign_extend) {
	return ReadU16LaneAsU32(operand, false, sign_extend);
}

IR::U32 Translator::Read16LaneBits(const Decoder::Operand& operand, bool high_lane) {
	auto raw_operand      = operand;
	raw_operand.sdwa_sel  = 6;
	raw_operand.sdwa_sext = false;
	auto bits             = ReadRawU32(operand);
	if (operand.kind == Decoder::OperandKind::FloatInlineConstant) {
		// Inline floats encode the operand's width before SDWA or OP_SEL selects bits.
		const auto half      = IR::F16(ir.Emit(IR::ValueOpcode::ConvertF16F32, {ir.BitCastF32(bits)}));
		const auto half_bits = IR::U16(ir.Emit(IR::ValueOpcode::BitCastU16F16, {half}));
		bits                = IR::U32(ir.Emit(IR::ValueOpcode::ConvertU32U16, {half_bits}));
	}
	bits = ApplyBitSourceModifiers(raw_operand, bits);
	uint32_t offset = (high_lane ? operand.op_sel_hi : operand.op_sel) ? 16u : 0u;
	uint32_t width  = 16u;
	if (operand.sdwa_sel <= 3u) {
		offset = operand.sdwa_sel * 8u;
		width  = 8u;
	} else if (operand.sdwa_sel == 4u || operand.sdwa_sel == 5u) {
		offset = operand.sdwa_sel == 5u ? 16u : 0u;
	}
	const auto extract = width == 8u && operand.sdwa_sext
	                         ? IR::ValueOpcode::BitFieldSExtract
	                         : IR::ValueOpcode::BitFieldUExtract;
	auto value = IR::U32(ir.Emit(extract, {bits, IR::Value(offset), IR::Value(width)}));
	if (width == 8u && operand.sdwa_sext) {
		value = ir.BitwiseAnd(value, IR::U32(IR::Value(0xffffu)));
	}
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
	switch (operand.kind) {
		case Decoder::OperandKind::ExecLo:
			ir.SetExec(ThreadBit(value));
			ir.SetExecLo(value[0]);
			ir.SetExecHi(value[1]);
			return;
		case Decoder::OperandKind::VccLo:
			ir.SetVcc(ThreadBit(value));
			ir.SetVccLo(value[0]);
			ir.SetVccHi(value[1]);
			return;
		case Decoder::OperandKind::Sgpr: break;
		default: break;
	}
	WriteRawU32(operand, value[0]);
	WriteRawU32(OffsetOperand(operand, 1), value[1]);
}

IR::U1 Translator::ThreadBit(const std::array<IR::U32, 2>& mask) {
	const auto lane = IR::U32(ir.Emit(IR::ValueOpcode::LaneId));
	const auto word = program.wave_size == 64u
	                      ? ir.Select(ir.ULessThan(lane, IR::U32(IR::Value(32u))), mask[0], mask[1])
	                      : mask[0];
	const auto bit  = ir.BitwiseAnd(lane, IR::U32(IR::Value(31u)));
	return ir.INotEqual(ir.BitwiseAnd(ir.ShiftRightLogical(word, bit), IR::U32(IR::Value(1u))),
	                    IR::U32(IR::Value(0u)));
}

IR::U32 Translator::ConditionBit(const Decoder::Operand& operand) {
	return ir.Select(ReadMask(operand), IR::U32(IR::Value(1u)), IR::U32(IR::Value(0u)));
}

IR::U1 Translator::ReadMask(const Decoder::Operand& operand) {
	if (operand.kind == Decoder::OperandKind::LiteralConstant ||
	    operand.kind == Decoder::OperandKind::IntegerInlineConstant ||
	    operand.kind == Decoder::OperandKind::FloatInlineConstant) {
		return ThreadBit(ReadU32Pair(operand));
	}
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: {
			const auto reg = static_cast<IR::ScalarReg>(operand.reg);
			const auto mask = program.wave_size == 64u
			                      ? ReadU32Pair(operand)
			                      : std::array {ReadRawU32(operand), IR::U32(IR::Value(0u))};
			return IR::U1(ir.Emit(
			    IR::ValueOpcode::SelectU1,
			    {ir.GetScalarMaskTag(reg), ir.GetThreadBitScalarReg(reg), ThreadBit(mask)}));
		}
		case Decoder::OperandKind::ExecLo:
		case Decoder::OperandKind::ExecHi: return ir.GetExec();
		case Decoder::OperandKind::VccLo:
		case Decoder::OperandKind::VccHi:
			return program.wave_size == 32u
			           ? ThreadBit({ReadRawU32(operand), IR::U32(IR::Value(0u))})
			           : ir.GetVcc();
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

std::array<IR::U32, 2> Translator::WriteMask(const Decoder::Operand& operand, IR::U1 value,
                                             bool write_64) {
	const auto mask = BallotMask(value);
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: {
			const auto reg  = static_cast<IR::ScalarReg>(operand.reg);
			ir.SetThreadBitScalarReg(reg, value);
			ir.SetScalarMaskTag(reg, IR::U1(IR::Value(true)));
			if (IR::RegIndex(reg) > 0u) {
				ir.SetScalarMaskTag(static_cast<IR::ScalarReg>(IR::RegIndex(reg) - 1u),
				                    IR::U1(IR::Value(false)));
			}
			ir.SetScalarReg(reg, mask[0]);
			program.scalar_writes.push_back({current_pc, reg});
			// A wave32 VALU mask destination must not overwrite the neighboring SGPR.
			if ((write_64 || program.wave_size == 64u) &&
			    IR::RegIndex(reg) + 1u < IR::NumScalarRegs) {
				const auto high = static_cast<IR::ScalarReg>(IR::RegIndex(reg) + 1u);
				ir.SetScalarReg(high, mask[1]);
				program.scalar_writes.push_back({current_pc, high});
				ir.SetThreadBitScalarReg(high, IR::U1(IR::Value(false)));
				ir.SetScalarMaskTag(high, IR::U1(IR::Value(false)));
			}
			return mask;
		}
		case Decoder::OperandKind::ExecLo:
		case Decoder::OperandKind::ExecHi: {
			ir.SetExec(value);
			ir.SetExecLo(mask[0]);
			ir.SetExecHi(mask[1]);
			return mask;
		}
		case Decoder::OperandKind::VccLo:
		case Decoder::OperandKind::VccHi: {
			if (!write_64 && program.wave_size == 32u) {
				WriteRawU32(operand, mask[0]);
				return mask;
			}
			ir.SetVcc(value);
			ir.SetVccLo(mask[0]);
			ir.SetVccHi(mask[1]);
			return mask;
		}
		case Decoder::OperandKind::Scc: ir.SetScc(value); return mask;
		default:
			WriteRawU32(operand, mask[0]);
			WriteRawU32(OffsetOperand(operand, 1), mask[1]);
			return mask;
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

void Translator::AddBranchCondition(const CFG::Graph& graph, const CFG::BasicBlock& source,
                                    IR::Block& info) {
	const auto native_condition = [&](CFG::BranchCondition kind) -> IR::U1 {
		IR::U1 condition;
		switch (kind) {
			case CFG::BranchCondition::Always: condition = IR::U1(IR::Value(true)); break;
			case CFG::BranchCondition::SccZero: condition = ir.LogicalNot(ir.GetScc()); break;
			case CFG::BranchCondition::SccNonZero: condition = ir.GetScc(); break;
			case CFG::BranchCondition::VccZero: condition = ir.LogicalNot(ir.GetVcc()); break;
			case CFG::BranchCondition::VccNonZero: condition = ir.GetVcc(); break;
			case CFG::BranchCondition::ExecZero: condition = ir.LogicalNot(ir.GetExec()); break;
			case CFG::BranchCondition::ExecNonZero: condition = ir.GetExec(); break;
			case CFG::BranchCondition::ScalarInstruction:
				EXIT_IF(instruction_branch_condition.IsEmpty());
				condition = instruction_branch_condition;
				break;
			default: EXIT("block %u has an invalid native branch condition", source.id);
		}
		return IR::U1(ir.Emit(IR::ValueOpcode::ConditionRef, {condition}, kind));
	};
	const auto expression = [&](auto&& self, uint32_t index) -> IR::U1 {
		const auto& value = graph.expressions.at(index);
		switch (value.op) {
			case CFG::ConditionExpression::Op::Constant: return IR::U1(IR::Value(value.lhs != 0));
			case CFG::ConditionExpression::Op::Variable: return ir.GetGotoVariable(value.lhs);
			case CFG::ConditionExpression::Op::Native:
				return native_condition(static_cast<CFG::BranchCondition>(value.lhs));
			case CFG::ConditionExpression::Op::Not: return ir.LogicalNot(self(self, value.lhs));
			case CFG::ConditionExpression::Op::Or:
				return ir.LogicalOr(self(self, value.lhs), self(self, value.rhs));
		}
		EXIT("invalid CFG condition expression");
	};
	for (const auto& assignment: source.assignments) {
		ir.SetGotoVariable(assignment.variable, expression(expression, assignment.expression));
	}
	const auto& term = source.terminator;
	if (term.kind == CFG::TerminatorKind::IndirectBranch) {
		if (term.indirect_selector_code != UINT32_MAX) {
			info.indirect_target = ReadScalarCode(term.indirect_selector_code);
		} else if (term.indirect_pc_sgpr != UINT32_MAX) {
			info.indirect_target = ir.GetScalarReg(static_cast<IR::ScalarReg>(term.indirect_pc_sgpr));
		} else {
			EXIT("block %u has no indirect branch selector", source.id);
		}
		ir.Emit(IR::ValueOpcode::ReferenceU32, {info.indirect_target});
		return;
	}
	if (term.kind == CFG::TerminatorKind::ConditionalBranch) {
		const auto condition = term.expression != UINT32_MAX
		                           ? expression(expression, term.expression)
		                           : native_condition(term.condition);
		info.condition = condition;
		ir.Emit(IR::ValueOpcode::Reference, {condition});
	}
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
	switch (inst.opcode) {
		case Decoder::Opcode::V_CVT_F64_I32:
		case Decoder::Opcode::V_CVT_F64_F32:
		case Decoder::Opcode::V_CVT_F64_U32: include_vector(inst.dst, 2u); break;
		case Decoder::Opcode::V_FMA_F64: include_vector(inst.src2, 2u); [[fallthrough]];
		case Decoder::Opcode::V_ADD_F64:
		case Decoder::Opcode::V_MIN_F64:
		case Decoder::Opcode::V_MAX_F64:
		case Decoder::Opcode::V_MUL_F64: include_vector(inst.src1, 2u); [[fallthrough]];
		case Decoder::Opcode::V_RCP_F64:
		case Decoder::Opcode::V_TRUNC_F64:
		case Decoder::Opcode::V_CEIL_F64:
		case Decoder::Opcode::V_FLOOR_F64:
		case Decoder::Opcode::V_FRACT_F64: include_vector(inst.dst, 2u); [[fallthrough]];
		case Decoder::Opcode::V_CVT_F32_F64: include_vector(inst.src0, 2u); break;
		case Decoder::Opcode::V_CMP_F_F64:
		case Decoder::Opcode::V_CMP_LT_F64:
		case Decoder::Opcode::V_CMP_EQ_F64:
		case Decoder::Opcode::V_CMP_LE_F64:
		case Decoder::Opcode::V_CMP_GT_F64:
		case Decoder::Opcode::V_CMP_LG_F64:
		case Decoder::Opcode::V_CMP_GE_F64:
		case Decoder::Opcode::V_CMP_O_F64:
		case Decoder::Opcode::V_CMP_U_F64:
		case Decoder::Opcode::V_CMP_NGE_F64:
		case Decoder::Opcode::V_CMP_NLG_F64:
		case Decoder::Opcode::V_CMP_NGT_F64:
		case Decoder::Opcode::V_CMP_NLE_F64:
		case Decoder::Opcode::V_CMP_NEQ_F64:
		case Decoder::Opcode::V_CMP_NLT_F64:
		case Decoder::Opcode::V_CMP_TRU_F64:
		case Decoder::Opcode::V_CMPX_F_F64:
		case Decoder::Opcode::V_CMPX_LT_F64:
		case Decoder::Opcode::V_CMPX_EQ_F64:
		case Decoder::Opcode::V_CMPX_LE_F64:
		case Decoder::Opcode::V_CMPX_GT_F64:
		case Decoder::Opcode::V_CMPX_LG_F64:
		case Decoder::Opcode::V_CMPX_GE_F64:
		case Decoder::Opcode::V_CMPX_O_F64:
		case Decoder::Opcode::V_CMPX_U_F64:
		case Decoder::Opcode::V_CMPX_NGE_F64:
		case Decoder::Opcode::V_CMPX_NLG_F64:
		case Decoder::Opcode::V_CMPX_NGT_F64:
		case Decoder::Opcode::V_CMPX_NLE_F64:
		case Decoder::Opcode::V_CMPX_NEQ_F64:
		case Decoder::Opcode::V_CMPX_NLT_F64:
		case Decoder::Opcode::V_CMPX_TRU_F64:
		case Decoder::Opcode::V_CMP_F_I64:
		case Decoder::Opcode::V_CMP_LT_I64:
		case Decoder::Opcode::V_CMP_EQ_I64:
		case Decoder::Opcode::V_CMP_LE_I64:
		case Decoder::Opcode::V_CMP_GT_I64:
		case Decoder::Opcode::V_CMP_NE_I64:
		case Decoder::Opcode::V_CMP_GE_I64:
		case Decoder::Opcode::V_CMP_T_I64:
		case Decoder::Opcode::V_CMP_F_U64:
		case Decoder::Opcode::V_CMP_LT_U64:
		case Decoder::Opcode::V_CMP_EQ_U64:
		case Decoder::Opcode::V_CMP_LE_U64:
		case Decoder::Opcode::V_CMP_GT_U64:
		case Decoder::Opcode::V_CMP_NE_U64:
		case Decoder::Opcode::V_CMP_GE_U64:
		case Decoder::Opcode::V_CMP_T_U64:
		case Decoder::Opcode::V_CMPX_F_I64:
		case Decoder::Opcode::V_CMPX_LT_I64:
		case Decoder::Opcode::V_CMPX_EQ_I64:
		case Decoder::Opcode::V_CMPX_LE_I64:
		case Decoder::Opcode::V_CMPX_GT_I64:
		case Decoder::Opcode::V_CMPX_NE_I64:
		case Decoder::Opcode::V_CMPX_GE_I64:
		case Decoder::Opcode::V_CMPX_T_I64:
		case Decoder::Opcode::V_CMPX_F_U64:
		case Decoder::Opcode::V_CMPX_LT_U64:
		case Decoder::Opcode::V_CMPX_EQ_U64:
		case Decoder::Opcode::V_CMPX_LE_U64:
		case Decoder::Opcode::V_CMPX_GT_U64:
		case Decoder::Opcode::V_CMPX_NE_U64:
		case Decoder::Opcode::V_CMPX_GE_U64:
		case Decoder::Opcode::V_CMPX_T_U64:
			include_vector(inst.src0, 2u);
			include_vector(inst.src1, 2u);
			break;
		default: break;
	}
	if (inst.family == Decoder::Family::DS) {
		switch (inst.opcode) {
			case Decoder::Opcode::DS_ADD_U64:
			case Decoder::Opcode::DS_OR_B64:
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

void ValidateTranslateOptions(const TranslateOptions& options) {
	if (options.wave_size != 32u && options.wave_size != 64u) {
		EXIT("shader translation requires wave32 or wave64, got %u", options.wave_size);
	}
	if (options.embedded_fetch != nullptr && options.stage != ShaderType::Vertex &&
	    options.stage != ShaderType::Local) {
		EXIT("embedded fetch requires a vertex or local shader");
	}
	switch (options.stage) {
		case ShaderType::Vertex:
		case ShaderType::Local:
		case ShaderType::TessellationControl:
		case ShaderType::TessellationEvaluation:
		case ShaderType::Mesh:
			if (options.input_info.vertex == nullptr) {
				EXIT("vertex shader translation has no vertex input metadata");
			}
			return;
		case ShaderType::Pixel:
			if (options.input_info.pixel == nullptr) {
				EXIT("pixel shader translation has no pixel input metadata");
			}
			return;
		case ShaderType::Compute:
			if (options.input_info.compute == nullptr) {
				EXIT("compute shader translation has no compute input metadata");
			}
			return;
		default:
			EXIT("shader translation has unsupported stage %u",
			     static_cast<uint32_t>(options.stage));
	}
}

} // namespace

IR::Program TranslateProgram(const Decoder::Program& decoded, const CFG::Graph& cfg,
                             const TranslateOptions& options) {
	ValidateTranslateOptions(options);
	if (cfg.blocks.empty()) {
		EXIT("cannot translate an empty CFG");
	}

	IR::Program result;
	result.stage               = options.stage;
	result.wave_size           = options.wave_size;
	result.shader_hash         = options.shader_hash;
	result.user_data_base      = options.user_data_base;
	result.user_data_count     = options.user_data_count;
	switch (options.stage) {
		case ShaderType::Vertex:
		case ShaderType::Local:
		case ShaderType::TessellationControl:
		case ShaderType::TessellationEvaluation:
			result.scratch_dwords = options.input_info.vertex->scratch_size_dwords;
			break;
		case ShaderType::Mesh:
			result.scratch_dwords = options.input_info.vertex->mesh.scratch_size_dwords;
			break;
		case ShaderType::Pixel:
			result.scratch_dwords = options.input_info.pixel->scratch_size_dwords;
			break;
		case ShaderType::Compute:
			result.scratch_dwords = options.input_info.compute->scratch_size_dwords;
			break;
		default: break; // ValidateTranslateOptions rejects unsupported stages.
	}
	result.dispatcher_fallback = cfg.irreducible || cfg.unsupported;
	result.cfg_failure_kind    = cfg.failure_kind;
	result.fallback_reason     = cfg.unsupported_reason;
	if (options.embedded_fetch != nullptr) {
		result.info.vertex_offset_sgpr   = options.embedded_fetch->vertex_offset_sgpr;
		result.info.instance_offset_sgpr = options.embedded_fetch->instance_offset_sgpr;
	}

	uint32_t vector_limit = 1u;
	for (const auto& cfg_block: cfg.blocks) {
		for (uint32_t index = cfg_block.inst_begin; index < cfg_block.inst_end; index++) {
			if (index >= decoded.instructions.size()) {
				EXIT("CFG block %u references instruction %u outside decoded program of size %zu",
				     cfg_block.id, index, decoded.instructions.size());
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
	const auto max_id = std::ranges::max_element(cfg.blocks, {}, [](const CFG::BasicBlock& block) {
		                    return block.id;
	                    })->id;
	if (max_id == UINT32_MAX) {
		EXIT("cannot allocate a typed entry block id");
	}
	result.block_storage.push_back(std::make_unique<IR::Block>());
	result.blocks.push_back(result.block_storage.back().get());
	auto& entry    = *result.blocks.front();
	entry.id       = max_id + 1u;
	entry.start_pc = entry.end_pc = cfg.blocks.front().start_pc;
	entry.terminator.kind         = CFG::TerminatorKind::Branch;

	std::unordered_map<uint32_t, IR::Block*> blocks_by_id;
	blocks_by_id.reserve(cfg.blocks.size());
	for (const auto& source: cfg.blocks) {
		auto& block = *result.block_storage.emplace_back(std::make_unique<IR::Block>());
		if (!blocks_by_id.emplace(source.id, &block).second) {
			EXIT("CFG contains duplicate block id %u", source.id);
		}
		result.blocks.push_back(&block);
		block.id       = source.id;
		block.start_pc = source.start_pc;
		block.end_pc   = source.end_pc;
	}
	const auto target_block = [&](uint32_t id) -> IR::Block* {
		return id == UINT32_MAX ? nullptr : blocks_by_id.at(id);
	};
	entry.terminator.true_block = target_block(cfg.blocks.front().id);
	for (const auto& source: cfg.blocks) {
		auto&       block               = *blocks_by_id.at(source.id);
		const auto& term                = source.terminator;
		block.terminator.kind           = term.kind;
		block.terminator.true_block     = target_block(term.true_block);
		block.terminator.false_block    = target_block(term.false_block);
		block.terminator.merge_block    = target_block(term.merge_block);
		block.terminator.continue_block = target_block(term.continue_block);
		block.terminator.loop_header    = term.loop_header;
		block.terminator.indexed        = term.indirect_selector_code != UINT32_MAX;
		const auto& values =
		    block.terminator.indexed ? term.indirect_selector_values : term.indirect_target_pcs;
		const auto& targets =
		    block.terminator.indexed ? term.indirect_selector_targets : term.indirect_targets;
		if (values.size() != targets.size()) {
			EXIT("CFG block %u has inconsistent indirect targets", source.id);
		}
		block.terminator.cases.reserve(values.size());
		for (size_t index = 0; index < values.size(); ++index) {
			block.terminator.cases.push_back({values[index], target_block(targets[index])});
		}
		for (const auto successor: source.successors) {
			const auto target = blocks_by_id.find(successor);
			if (target == blocks_by_id.end()) {
				EXIT("CFG block %u has unknown successor %u", source.id, successor);
			}
			block.AddBranch(target->second);
		}
	}
	{
		entry.AddBranch(entry.terminator.true_block);
		IR::IREmitter entry_ir(&entry);
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
			entry_ir.SetScalarMaskTag(reg, IR::U1(IR::Value(false)));
		}
		auto                 initial_exec  = IR::U1(IR::Value(true));
		uint32_t             total_threads = 0;
		const auto*          workgroup = ShaderWorkgroupInput(options.stage, options.input_info);
		if (workgroup != nullptr) {
			total_threads = std::max(workgroup->threads_num[0], 1u) *
			                std::max(workgroup->threads_num[1], 1u) *
			                std::max(workgroup->threads_num[2], 1u);
			if (options.wave_size == 64u && workgroup->host_subgroup_size == 32u &&
			    total_threads % 64u != 0u) {
				initial_exec = entry_ir.ULessThan(builtin(IR::StageInputKind::LocalInvocationIndex),
				                                  IR::U32(IR::Value(total_threads)));
			}
		}
		if (options.stage == ShaderType::Compute &&
		    options.input_info.compute->dispatch_thread_dimensions) {
			for (uint32_t axis = 0; axis < 3u; axis++) {
				const auto extent = IR::U32(entry_ir.Emit(
				    IR::ValueOpcode::GetDispatchThreadExtent, {IR::Value(axis)}));
				initial_exec = entry_ir.LogicalAnd(
				    initial_exec,
				    entry_ir.ULessThan(builtin(IR::StageInputKind::GlobalInvocationId, axis), extent));
			}
		}
		entry_ir.SetExec(initial_exec);
		const auto initial_mask = entry_ir.Emit(IR::ValueOpcode::Ballot, {initial_exec});
		entry_ir.SetExecLo(entry_ir.CompositeExtract(initial_mask, 0));
		entry_ir.SetExecHi(options.wave_size == 64u ? entry_ir.CompositeExtract(initial_mask, 1)
		                                            : IR::U32(IR::Value(0u)));
		if (options.stage == ShaderType::Compute) {
			const auto* cs = options.input_info.compute;
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
		} else if (options.stage == ShaderType::Mesh) {
			const auto& mesh = options.input_info.vertex->mesh;
			EXIT_NOT_IMPLEMENTED(mesh.primitives_per_group == 0u || mesh.vertices_per_group > 0x1ffu ||
			                     mesh.primitives_per_group > 0x1ffu ||
			                     total_threads > 15u * options.wave_size);
			const auto u32  = [](uint32_t value) { return IR::U32(IR::Value(value)); };
			const auto draw = [&](uint32_t index) {
				return IR::U32(
				    entry_ir.Emit(IR::ValueOpcode::MeshDrawParameter, {IR::Value(index)}));
			};
			const auto minimum = [&](IR::U32 lhs, IR::U32 rhs) {
				return IR::U32(entry_ir.Emit(IR::ValueOpcode::UMin32, {lhs, rhs}));
			};
			const auto subtract_saturate = [&](IR::U32 lhs, IR::U32 rhs) {
				return entry_ir.ISub(lhs, minimum(lhs, rhs));
			};
			const auto local = builtin(IR::StageInputKind::LocalInvocationIndex);
			const auto group = builtin(IR::StageInputKind::WorkgroupId, 0);
			const auto primitive_chunk = mesh.fast_launch ? group :
			    entry_ir.IMul(group, u32(mesh.primitives_per_group));
			const auto step  = u32(mesh.InputPrimitiveStep());
			const auto size  = u32(mesh.InputPrimitiveSize());
			const auto chunk = mesh.fast_launch ? group : entry_ir.IMul(primitive_chunk, step);
			const auto vertices = mesh.fast_launch ? u32(mesh.vertices_per_group) :
			    minimum(subtract_saturate(draw(0), chunk), u32(mesh.vertices_per_group));
			const auto primitives = entry_ir.Select(
			    entry_ir.ULessThan(vertices, size), u32(0),
			    entry_ir.IAdd(IR::U32(entry_ir.Emit(IR::ValueOpcode::UDiv32,
			                                       {subtract_saturate(vertices, size), step})),
			                  u32(1)));
			const auto wave = entry_ir.ShiftRightLogical(local, u32(options.wave_size == 32u ? 5u : 6u));
			const auto wave_base = entry_ir.BitwiseAnd(local, u32(~(options.wave_size - 1u)));
			const auto vertex_count =
			    minimum(subtract_saturate(vertices, wave_base), u32(options.wave_size));
			const auto primitive_count =
			    minimum(subtract_saturate(primitives, wave_base), u32(options.wave_size));
			const auto wave_info = entry_ir.BitwiseOr(entry_ir.ShiftLeftLogical(wave, u32(24)),
			                                          u32(((total_threads + options.wave_size - 1u) /
			                                               options.wave_size) << 28u));
			entry_ir.SetScalarReg(
			    static_cast<IR::ScalarReg>(2),
			    entry_ir.BitwiseOr(entry_ir.ShiftLeftLogical(vertices, u32(12)),
			                       entry_ir.ShiftLeftLogical(primitives, u32(22))));
			entry_ir.SetScalarReg(
			    static_cast<IR::ScalarReg>(3),
			    entry_ir.BitwiseOr(wave_info, entry_ir.BitwiseOr(entry_ir.ShiftLeftLogical(
			                                                         primitive_count, u32(8)),
			                                                     vertex_count)));
			if (mesh.fast_launch) {
				// Fast launch broadcasts the subgroup's base vertex and instance to every lane.
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(5), entry_ir.IAdd(draw(1), group));
				entry_ir.SetVectorReg(
				    static_cast<IR::VectorReg>(6),
				    entry_ir.IAdd(draw(2), builtin(IR::StageInputKind::WorkgroupId, 1)));
			} else {
				// GS adjacency addresses local ES records in LDS. Fans retain the draw's
				// center in every subgroup; strip winding follows the global primitive.
				const auto vertex = entry_ir.IMul(local, step);
				auto       first  = vertex;
				auto       second = u32(0);
				auto       third  = u32(0);
				if (mesh.InputPrimitiveSize() >= 2u) {
					second = entry_ir.IAdd(vertex, u32(1));
				}
				if (mesh.InputPrimitiveSize() == 3u) {
					third = entry_ir.IAdd(vertex, u32(2));
				}
				auto input_vertex = entry_ir.IAdd(chunk, local);
				if (mesh.input_primitive == static_cast<uint32_t>(Prospero::PrimitiveType::kTriFan)) {
					first = u32(0);
					input_vertex = entry_ir.Select(entry_ir.IEqual(local, u32(0)), u32(0), input_vertex);
				} else if (mesh.input_primitive == static_cast<uint32_t>(Prospero::PrimitiveType::kTriStrip)) {
					const auto parity = entry_ir.BitwiseAnd(entry_ir.IAdd(primitive_chunk, local), u32(1));
					first = entry_ir.IAdd(first, parity);
					second = entry_ir.ISub(second, parity);
				}
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(0),
				                      entry_ir.BitwiseOr(entry_ir.ShiftLeftLogical(first, u32(2)),
				                                         entry_ir.ShiftLeftLogical(second, u32(18))));
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(1),
				                      entry_ir.ShiftLeftLogical(third, u32(2)));
				const auto index_bytes  = draw(3);
				const auto indexed      = entry_ir.INotEqual(index_bytes, u32(0));
				const auto index_low    = draw(4);
				const auto byte_offset  = entry_ir.IAdd(entry_ir.BitwiseAnd(index_low, u32(3)),
				                                           entry_ir.IMul(input_vertex, index_bytes));
				const auto index_resource = entry_ir.Emit(
				    IR::ValueOpcode::GetAddressResource,
				    {entry_ir.BitwiseAnd(index_low, u32(~3u)), draw(5)});
				const auto memory_index = static_cast<uint32_t>(result.memory_info.size());
				result.memory_info.push_back({.kind = IR::ResourceKind::Global});
				const auto packed_index = entry_ir.Emit(
				    IR::ValueOpcode::LoadAddressU32,
				    {index_resource, entry_ir.BitwiseAnd(byte_offset, u32(~3u)), u32(0),
				     entry_ir.LogicalAnd(indexed, entry_ir.ULessThan(local, vertices))},
				    IR::MemoryFlags {.index = memory_index});
				const auto index = IR::U32(entry_ir.Emit(
				    IR::ValueOpcode::BitFieldUExtract,
				    {packed_index, entry_ir.IMul(entry_ir.BitwiseAnd(byte_offset, u32(3)), u32(8)),
				     entry_ir.IMul(index_bytes, u32(8))}));
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(5),
				                      entry_ir.IAdd(draw(1), entry_ir.Select(indexed, index, input_vertex)));
				entry_ir.SetVectorReg(
				    static_cast<IR::VectorReg>(8),
				    entry_ir.IAdd(draw(2), builtin(IR::StageInputKind::WorkgroupId, 1)));
			}
		} else if (options.stage == ShaderType::Local) {
			entry_ir.SetScalarReg(static_cast<IR::ScalarReg>(3), IR::U32(IR::Value(64u)));
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(2),
			                      builtin(IR::StageInputKind::VertexIndex));
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(3), IR::U32(IR::Value(0u)));
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(5),
			                      builtin(IR::StageInputKind::InstanceIndex));
		} else if (options.stage == ShaderType::TessellationControl) {
			const auto& tess = options.input_info.vertex->tess;
			entry_ir.SetScalarReg(
			    static_cast<IR::ScalarReg>(2),
			    IR::U32(entry_ir.Emit(IR::ValueOpcode::TessellationBase, {IR::Value(0u)})));
			entry_ir.SetScalarReg(
			    static_cast<IR::ScalarReg>(4),
			    IR::U32(entry_ir.Emit(IR::ValueOpcode::TessellationBase, {IR::Value(1u)})));
			entry_ir.SetScalarReg(static_cast<IR::ScalarReg>(3),
			                      IR::U32(IR::Value(0x81010000u | tess.input_control_points |
			                                        (tess.output_control_points << 8u))));
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(0),
			                      builtin(IR::StageInputKind::PrimitiveId));
			entry_ir.SetVectorReg(
			    static_cast<IR::VectorReg>(1),
			    entry_ir.ShiftLeftLogical(builtin(IR::StageInputKind::InvocationId),
			                              IR::U32(IR::Value(8u))));
		} else if (options.stage == ShaderType::TessellationEvaluation) {
			entry_ir.SetScalarReg(static_cast<IR::ScalarReg>(3), IR::U32(IR::Value(64u)));
			entry_ir.SetScalarReg(
			    static_cast<IR::ScalarReg>(4),
			    IR::U32(entry_ir.Emit(IR::ValueOpcode::TessellationBase, {IR::Value(0u)})));
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(5),
			                      builtin(IR::StageInputKind::TessCoord, 0));
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(6),
			                      builtin(IR::StageInputKind::TessCoord, 1));
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(7), IR::U32(IR::Value(0u)));
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(8),
			                      builtin(IR::StageInputKind::PrimitiveId));
		} else if (options.stage == ShaderType::Pixel) {
			const auto* ps = options.input_info.pixel;
			const auto barycentric_pair = [&](uint32_t reg, IR::StageInputKind kind) {
				if (reg != UINT32_MAX) {
					entry_ir.SetVectorReg(static_cast<IR::VectorReg>(reg), builtin(kind, 0));
					entry_ir.SetVectorReg(static_cast<IR::VectorReg>(reg + 1u), builtin(kind, 1));
				}
			};
			barycentric_pair(ps->ps_perspective_center_vgpr, IR::StageInputKind::BaryCoordSmooth);
			barycentric_pair(ps->ps_perspective_sample_vgpr,
			                 IR::StageInputKind::BaryCoordSmoothSample);
			barycentric_pair(ps->ps_perspective_centroid_vgpr,
			                 IR::StageInputKind::BaryCoordSmoothCentroid);
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
				const auto reciprocal_w = entry_ir.BitCastF32(builtin(IR::StageInputKind::FragCoord, 3));
				const auto w = IR::F32(entry_ir.Emit(IR::ValueOpcode::FPRecip32, {reciprocal_w}));
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(reg++),
				                      entry_ir.BitCastU32(w));
			}
			if (ps->ps_front_face) {
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(reg++),
				                      builtin(IR::StageInputKind::FrontFacing));
			}
			if (ps->ps_ancillary) {
				entry_ir.SetVectorReg(static_cast<IR::VectorReg>(reg),
				                      builtin(IR::StageInputKind::PackedAncillary));
			}
		} else if (options.stage == ShaderType::Vertex) {
			// Vulkan owns primitive assembly; each vertex subgroup is one NGG wave.
			// Keep its full lane extent: mbcnt(-1) uses lane ordinals, not active counts.
			entry_ir.SetScalarReg(static_cast<IR::ScalarReg>(2),
			                      IR::U32(IR::Value(options.wave_size << 12u)));
			entry_ir.SetScalarReg(static_cast<IR::ScalarReg>(3),
			                      IR::U32(IR::Value((1u << 28u) | options.wave_size)));
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(5),
			                      builtin(IR::StageInputKind::VertexIndex));
			entry_ir.SetVectorReg(static_cast<IR::VectorReg>(8),
			                      builtin(IR::StageInputKind::InstanceIndex));
		}
	}
	const bool flush_f32_inputs = options.stage == ShaderType::Compute &&
	                              (options.input_info.compute->float_mode & 0x10u) == 0;
	for (const auto& cfg_block: cfg.blocks) {
		auto*      block = blocks_by_id.at(cfg_block.id);
		Translator translator(result, block, vector_limit, flush_f32_inputs,
		    options.stage == ShaderType::Compute && !options.input_info.compute->async_compute);
		for (uint32_t index = cfg_block.inst_begin; index < cfg_block.inst_end; index++) {
			const auto& instruction = decoded.instructions[index];
			if (IsCodeTableLoad(cfg, instruction.pc)) {
				continue;
			}
			const auto* embedded = FindEmbeddedFetchLoad(options.embedded_fetch, instruction.pc);
			if (embedded != nullptr && IsBufferDwordLoad(instruction.opcode) &&
			    instruction.data_dwords == embedded->components &&
			    instruction.dst.kind == Decoder::OperandKind::Vgpr) {
				const auto resource =
				    ResolveEmbeddedFetchResource(*options.input_info.vertex, *embedded);
				if (resource < 0 || resource >= options.input_info.vertex->resources_num) {
					EXIT("embedded vertex fetch at 0x%08x has no resource for attribute %d",
					     instruction.pc, embedded->attrib_id);
				}
				translator.TranslateEmbeddedFetch(instruction, static_cast<uint32_t>(resource),
				                                  embedded->components,
				                                  options.input_info.vertex->resources[resource]);
				continue;
			}
			translator.TranslateInstruction(instruction);
		}
		translator.AddBranchCondition(cfg, cfg_block, *block);
	}
	IR::ValidateProgram(result, false);
	return result;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
