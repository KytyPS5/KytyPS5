#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {

IR::F32 Translator::SelectF32(IR::U1 condition, IR::F32 true_value, IR::F32 false_value) {
	return IR::F32(ir.Emit(IR::ValueOpcode::SelectF32, {condition, true_value, false_value}));
}

void Translator::V_CVT_F32_UBYTE(const Decoder::Instruction& inst, uint32_t byte_index) {
	const auto source = ReadU32(SourceAt(inst, 0));
	const auto byte   = ir.Emit(IR::ValueOpcode::BitFieldUExtract,
	                            {source, IR::Value(byte_index * 8u), IR::Value(8u)});
	WriteOperand(DestinationOperand(inst), ir.Emit(IR::ValueOpcode::ConvertF32U32, {byte}));
}

void Translator::V_CVT_F32_U32(const Decoder::Instruction& inst) {
	WriteOperand(DestinationOperand(inst),
	             ir.Emit(IR::ValueOpcode::ConvertF32U32, {ReadU32(SourceAt(inst, 0))}));
}

void Translator::V_CVT_F32_I32(const Decoder::Instruction& inst) {
	WriteOperand(DestinationOperand(inst),
	             ir.Emit(IR::ValueOpcode::ConvertF32S32, {ReadU32(SourceAt(inst, 0))}));
}

void Translator::V_CVT_U32_F32(const Decoder::Instruction& inst) {
	const auto value = IR::F32(ReadOperand(SourceAt(inst, 0), IR::Type::F32));
	WriteOperand(DestinationOperand(inst), ir.Emit(IR::ValueOpcode::ConvertU32F32, {value}));
}

void Translator::V_CVT_I32_F32(const Decoder::Instruction& inst) {
	const auto value = IR::F32(ReadOperand(SourceAt(inst, 0), IR::Type::F32));
	WriteOperand(DestinationOperand(inst), ir.Emit(IR::ValueOpcode::ConvertS32F32, {value}));
}

void Translator::V_CVT_F16_F32(const Decoder::Instruction& inst) {
	WriteF16(DestinationOperand(inst), IR::F32(ReadOperand(SourceAt(inst, 0), IR::Type::F32)));
}

void Translator::V_CVT_F32_F16(const Decoder::Instruction& inst) {
	WriteOperand(DestinationOperand(inst), ReadF16AsF32(inst.src0));
}

void Translator::V_CVT_F16_16(const Decoder::Instruction& inst, bool signed_value) {
	const auto value = IR::F32(
	    ir.Emit(signed_value ? IR::ValueOpcode::ConvertF32S32 : IR::ValueOpcode::ConvertF32U32,
	            {ReadU16AsU32(inst.src0, signed_value)}));
	WriteF16(DestinationOperand(inst), value);
}

void Translator::V_CVT_16_F16(const Decoder::Instruction& inst, bool signed_value) {
	const auto value = ReadF16AsF32(inst.src0);
	// The 32-bit conversion already saturates and maps NaN to zero.
	if (signed_value) {
		const auto converted = ir.Emit(IR::ValueOpcode::ConvertS32F32, {value});
		const auto low = ir.Emit(IR::ValueOpcode::SMax32, {converted, IR::Value(0xffff8000u)});
		Write16Bits(DestinationOperand(inst),
		            IR::U32(ir.Emit(IR::ValueOpcode::SMin32, {low, IR::Value(0x7fffu)})));
		return;
	}
	const auto converted = ir.Emit(IR::ValueOpcode::ConvertU32F32, {value});
	Write16Bits(DestinationOperand(inst),
	            IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {converted, IR::Value(0xffffu)})));
}

void Translator::V_CVT_RPI_I32_F32(const Decoder::Instruction& inst) {
	const auto source  = IR::F32(ReadOperand(SourceAt(inst, 0), IR::Type::F32));
	const auto biased  = IR::F32(ir.Emit(IR::ValueOpcode::FPAdd32, {source, IR::Value::F32(0.5f)}));
	const auto rounded = IR::F32(ir.Emit(IR::ValueOpcode::FPFloor32, {biased}));
	WriteOperand(DestinationOperand(inst), ir.Emit(IR::ValueOpcode::ConvertS32F32, {rounded}));
}

void Translator::V_CVT_FLR_I32_F32(const Decoder::Instruction& inst) {
	const auto source  = IR::F32(ReadOperand(SourceAt(inst, 0), IR::Type::F32));
	const auto rounded = IR::F32(ir.Emit(IR::ValueOpcode::FPFloor32, {source}));
	WriteOperand(DestinationOperand(inst), ir.Emit(IR::ValueOpcode::ConvertS32F32, {rounded}));
}

void Translator::V_FREXP_EXP_I32_F32(const Decoder::Instruction& inst) {
	const auto source = IR::F32(ReadOperand(SourceAt(inst, 0), IR::Type::F32));
	const auto bits   = ir.BitCastU32(source);
	const auto exponent =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {bits, IR::Value(23u), IR::Value(8u)}));
	const auto mantissa  = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x007fffffu)));
	const auto normal    = ir.ISub(exponent, IR::U32(IR::Value(126u)));
	const auto msb       = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb32, {mantissa}));
	const auto subnormal = ir.ISub(msb, IR::U32(IR::Value(148u)));
	const auto denormal  = ir.Select(ir.INotEqual(mantissa, IR::U32(IR::Value(0u))), subnormal,
	                                 IR::U32(IR::Value(0u)));
	const auto finite =
	    ir.Select(ir.INotEqual(exponent, IR::U32(IR::Value(0xffu))),
	              ir.Select(ir.INotEqual(exponent, IR::U32(IR::Value(0u))), normal, denormal),
	              IR::U32(IR::Value(0u)));
	WriteOperand(DestinationOperand(inst), finite);
}

void Translator::V_CVT_OFF_F32_I4(const Decoder::Instruction& inst) {
	const auto nibble =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldSExtract,
	                    {ReadU32(SourceAt(inst, 0)), IR::Value(0u), IR::Value(4u)}));
	const auto value = IR::F32(ir.Emit(IR::ValueOpcode::ConvertF32S32, {nibble}));
	WriteOperand(DestinationOperand(inst),
	             ir.Emit(IR::ValueOpcode::FPMul32, {value, IR::Value::F32(1.0f / 16.0f)}));
}

void Translator::V_CVT_PKRTZ_F16_F32(const Decoder::Instruction& inst) {
	const auto lhs = ApplyF32ResultModifiers(
	    inst.dst, IR::F32(ReadOperand(SourceAt(inst, 0), IR::Type::F32)));
	const auto rhs = ApplyF32ResultModifiers(
	    inst.dst, IR::F32(ReadOperand(SourceAt(inst, 1), IR::Type::F32)));
	WriteOperand(DestinationOperand(inst), ir.Emit(IR::ValueOpcode::PackFloat2x16Rtz, {lhs, rhs}));
}

void Translator::V_CVT_PKNORM_F32(const Decoder::Instruction& inst, IR::ValueOpcode opcode) {
	const auto lhs  = ReadOperand(SourceAt(inst, 0), IR::Type::F32);
	const auto rhs  = ReadOperand(SourceAt(inst, 1), IR::Type::F32);
	const auto pair = ir.Emit(IR::ValueOpcode::CompositeConstructF32x2, {lhs, rhs});
	WriteOperand(DestinationOperand(inst), ir.Emit(opcode, {pair}));
}

void Translator::V_CVT_PK_U8_F32(const Decoder::Instruction& inst) {
	const auto source = IR::F32(ReadOperand(SourceAt(inst, 0), IR::Type::F32));
	const auto byte =
	    IR::U32(ir.Emit(IR::ValueOpcode::UMin32,
		                {ir.Emit(IR::ValueOpcode::ConvertU32F32, {source}), IR::Value(255u)}));
	const auto index  = ir.BitwiseAnd(ReadU32(SourceAt(inst, 1)), IR::U32(IR::Value(3u)));
	const auto shift  = ir.ShiftLeftLogical(index, IR::U32(IR::Value(3u)));
	WriteOperand(DestinationOperand(inst),
	             ir.Emit(IR::ValueOpcode::BitFieldInsert,
	                     {ReadU32(SourceAt(inst, 2)), byte, shift, IR::Value(8u)}));
}

void Translator::V_PACK_B32_F16(const Decoder::Instruction& inst) {
	WriteOperand(DestinationOperand(inst),
	             PackU16Lanes(Read16LaneBits(inst.src0, false), Read16LaneBits(inst.src1, false)));
}

IR::U32 Translator::PackU16Lanes(IR::U32 low, IR::U32 high) {
	return IR::U32(
	    ir.Emit(IR::ValueOpcode::BitFieldInsert, {low, high, IR::Value(16u), IR::Value(16u)}));
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
