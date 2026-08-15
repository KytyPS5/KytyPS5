#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail {

IR::F32 Translator::SelectF32(IR::U1 condition, IR::F32 true_value, IR::F32 false_value) {
	return IR::F32(ir.Emit(IR::ValueOpcode::SelectF32, {condition, true_value, false_value}));
}

IR::U32 Translator::ConvertF32ToU32Saturated(IR::F32 value, float upper_bound, float safe_upper,
                                             uint32_t upper_result) {
	const auto zero = IR::F32(IR::Value::F32(0.0f));
	const auto nan  = IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {value}));
	const auto low  = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThanEqual32, {value, zero}));
	const auto high = IR::U1(
	    ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {value, IR::Value::F32(upper_bound)}));
	const auto truncated = IR::F32(ir.Emit(IR::ValueOpcode::FPTrunc32, {value}));
	const auto safe_low  = SelectF32(ir.LogicalOr(nan, low), zero, truncated);
	const auto safe      = SelectF32(high, IR::F32(IR::Value::F32(safe_upper)), safe_low);
	const auto converted = IR::U32(ir.Emit(IR::ValueOpcode::ConvertU32F32, {safe}));
	return ir.Select(high, IR::U32(IR::Value(upper_result)), converted);
}

IR::U32 Translator::ConvertF32ToI32Saturated(IR::F32 value, float lower_bound, float upper_bound,
                                             float safe_upper, uint32_t lower_result,
                                             uint32_t upper_result) {
	const auto nan = IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {value}));
	const auto low = IR::U1(
	    ir.Emit(IR::ValueOpcode::FPOrdLessThanEqual32, {value, IR::Value::F32(lower_bound)}));
	const auto high = IR::U1(
	    ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {value, IR::Value::F32(upper_bound)}));
	const auto truncated    = IR::F32(ir.Emit(IR::ValueOpcode::FPTrunc32, {value}));
	const auto safe_low     = SelectF32(low, IR::F32(IR::Value::F32(lower_bound)), truncated);
	const auto safe_high    = SelectF32(high, IR::F32(IR::Value::F32(safe_upper)), safe_low);
	const auto safe         = SelectF32(nan, IR::F32(IR::Value::F32(0.0f)), safe_high);
	const auto converted    = IR::U32(ir.Emit(IR::ValueOpcode::ConvertS32F32, {safe}));
	const auto clamped_high = ir.Select(high, IR::U32(IR::Value(upper_result)), converted);
	const auto clamped      = ir.Select(low, IR::U32(IR::Value(lower_result)), clamped_high);
	return ir.Select(nan, IR::U32(IR::Value(0u)), clamped);
}

bool Translator::TranslateConversion(const IR::Instruction& inst) {
	const auto f32 = [&](uint32_t index) {
		return IR::F32(ReadOperand(inst.src[index], IR::Type::F32));
	};
	const auto u32         = [&](uint32_t index) { return ReadU32(inst.src[index]); };
	const auto convert_u32 = [&](IR::F32 value) {
		return ConvertF32ToU32Saturated(value, 4294967296.0f, 4294967040.0f, 0xffffffffu);
	};
	const auto convert_i32 = [&](IR::F32 value) {
		return ConvertF32ToI32Saturated(value, -2147483648.0f, 2147483648.0f, 2147483520.0f,
		                                0x80000000u, 0x7fffffffu);
	};

	switch (inst.op) {
		case IR::Opcode::ConvertByteU32ToF32: {
			const auto index =
			    inst.src[1].kind == IR::OperandKind::ImmediateU32 ? inst.src[1].imm & 3u : 0u;
			const auto byte =
			    ir.BitwiseAnd(ir.ShiftRightLogical(u32(0), IR::U32(IR::Value(index * 8u))),
			                  IR::U32(IR::Value(0xffu)));
			WriteOperand(inst.dst, ir.Emit(IR::ValueOpcode::ConvertF32U32, {byte}));
			return true;
		}
		case IR::Opcode::ConvertU32ToF32:
			WriteOperand(inst.dst, ir.Emit(IR::ValueOpcode::ConvertF32U32, {u32(0)}));
			return true;
		case IR::Opcode::ConvertI32ToF32:
			WriteOperand(inst.dst, ir.Emit(IR::ValueOpcode::ConvertF32S32, {u32(0)}));
			return true;
		case IR::Opcode::ConvertF32ToU32: WriteOperand(inst.dst, convert_u32(f32(0))); return true;
		case IR::Opcode::ConvertF32ToI32: WriteOperand(inst.dst, convert_i32(f32(0))); return true;
		case IR::Opcode::ConvertF32ToF16: WriteF16(inst.dst, f32(0)); return true;
		case IR::Opcode::ConvertF16ToF32:
			WriteOperand(inst.dst, ReadF16AsF32(inst.src[0]));
			return true;
		case IR::Opcode::ConvertU16ToF16: {
			const auto value = IR::F32(
			    ir.Emit(IR::ValueOpcode::ConvertF32U32, {ReadU16AsU32(inst.src[0], false)}));
			WriteF16(inst.dst, value);
			return true;
		}
		case IR::Opcode::ConvertI16ToF16: {
			const auto value =
			    IR::F32(ir.Emit(IR::ValueOpcode::ConvertF32S32, {ReadU16AsU32(inst.src[0], true)}));
			WriteF16(inst.dst, value);
			return true;
		}
		case IR::Opcode::ConvertF16ToU16: {
			const auto converted =
			    ConvertF32ToU32Saturated(ReadF16AsF32(inst.src[0]), 65536.0f, 65535.0f, 0xffffu);
			WriteU16(inst.dst, converted);
			return true;
		}
		case IR::Opcode::ConvertF16ToI16: {
			const auto converted = ConvertF32ToI32Saturated(
			    ReadF16AsF32(inst.src[0]), -32768.0f, 32768.0f, 32767.0f, 0xffff8000u, 0x7fffu);
			WriteU16(inst.dst, ir.BitwiseAnd(converted, IR::U32(IR::Value(0xffffu))));
			return true;
		}
		case IR::Opcode::ConvertRoundPlusInfF32ToI32: {
			const auto biased =
			    IR::F32(ir.Emit(IR::ValueOpcode::FPAdd32, {f32(0), IR::Value::F32(0.5f)}));
			const auto rounded = IR::F32(ir.Emit(IR::ValueOpcode::FPFloor32, {biased}));
			WriteOperand(inst.dst, convert_i32(rounded));
			return true;
		}
		case IR::Opcode::ConvertFloorF32ToI32: {
			const auto rounded = IR::F32(ir.Emit(IR::ValueOpcode::FPFloor32, {f32(0)}));
			WriteOperand(inst.dst, convert_i32(rounded));
			return true;
		}
		case IR::Opcode::ConvertI4ToOffsetF32: {
			const auto nibble = IR::U32(
			    ir.Emit(IR::ValueOpcode::BitFieldSExtract, {u32(0), IR::Value(0u), IR::Value(4u)}));
			const auto value = IR::F32(ir.Emit(IR::ValueOpcode::ConvertF32S32, {nibble}));
			WriteOperand(inst.dst,
			             ir.Emit(IR::ValueOpcode::FPMul32, {value, IR::Value::F32(1.0f / 16.0f)}));
			return true;
		}
		case IR::Opcode::PackF32ToF16Rtz:
			WriteOperand(inst.dst, ir.Emit(IR::ValueOpcode::PackFloat2x16Rtz, {f32(0), f32(1)}));
			return true;
		case IR::Opcode::PackSnorm2x16F32:
		case IR::Opcode::PackUnorm2x16F32: {
			const auto pair   = ir.Emit(IR::ValueOpcode::CompositeConstructF32x2, {f32(0), f32(1)});
			const auto opcode = inst.op == IR::Opcode::PackSnorm2x16F32
			                        ? IR::ValueOpcode::PackSnorm2x16
			                        : IR::ValueOpcode::PackUnorm2x16;
			WriteOperand(inst.dst, ir.Emit(opcode, {pair}));
			return true;
		}
		case IR::Opcode::PackU8F32: {
			const auto byte  = ConvertF32ToU32Saturated(f32(0), 255.0f, 255.0f, 255u);
			const auto index = ir.BitwiseAnd(u32(1), IR::U32(IR::Value(3u)));
			const auto shift = ir.ShiftLeftLogical(index, IR::U32(IR::Value(3u)));
			const auto mask  = ir.ShiftLeftLogical(IR::U32(IR::Value(0xffu)), shift);
			const auto base  = ir.BitwiseAnd(u32(2), ir.BitwiseNot(mask));
			WriteOperand(inst.dst, ir.BitwiseOr(base, ir.ShiftLeftLogical(byte, shift)));
			return true;
		}
		case IR::Opcode::PackB32F16: {
			const auto low = ReadF16LaneBits(inst.src[0], false);
			const auto high =
			    ir.ShiftLeftLogical(ReadF16LaneBits(inst.src[1], false), IR::U32(IR::Value(16u)));
			WriteOperand(inst.dst, ir.BitwiseOr(low, high));
			return true;
		}
		default: return false;
	}
}

IR::U32 Translator::PackU16Lanes(IR::U32 low, IR::U32 high) {
	const auto mask = IR::U32(IR::Value(0xffffu));
	return ir.BitwiseOr(ir.BitwiseAnd(low, mask),
	                    ir.ShiftLeftLogical(ir.BitwiseAnd(high, mask), IR::U32(IR::Value(16u))));
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail
