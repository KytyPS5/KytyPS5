#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail {

bool Translator::TranslateU64MaskOperation(const IR::Instruction& inst) {
	if (current_per_invocation_masks) {
		return TranslatePerInvocationU64Mask(inst);
	}
	return inst.op == IR::Opcode::BitwiseAndU64 || inst.op == IR::Opcode::BitwiseOrU64 ||
	               inst.op == IR::Opcode::BitwiseXorU64 || inst.op == IR::Opcode::BitwiseNotU64
	           ? TranslateSimpleInteger(inst)
	           : TranslateComposedInteger(inst);
}

bool Translator::TranslateInstruction(const IR::Instruction& inst) {
	switch (IR::GetOpcodeInfo(inst.op).lowering_class) {
		case IR::LoweringClass::Control: return TranslateControlOperation(inst);
		case IR::LoweringClass::Move: return TranslateMove(inst);
		case IR::LoweringClass::Lane: return TranslateLaneOperation(inst);
		case IR::LoweringClass::State: return TranslateStateOperation(inst);
		case IR::LoweringClass::Memory: return TranslateMemoryOperation(inst);
		case IR::LoweringClass::Attribute: return TranslateAttributeOperation(inst);
		case IR::LoweringClass::IntegerCompare: return TranslateIntegerCompare(inst);
		case IR::LoweringClass::Integer16Compare: return TranslateInteger16Compare(inst);
		case IR::LoweringClass::FloatCompare: return TranslateFloatCompare(inst);
		case IR::LoweringClass::Conversion: return TranslateConversion(inst);
		case IR::LoweringClass::Integer16: return TranslateInteger16Operation(inst);
		case IR::LoweringClass::PackedInteger16: return TranslatePackedInteger16(inst);
		case IR::LoweringClass::PackedFloat16: return TranslatePackedFloat16(inst);
		case IR::LoweringClass::Float16: return TranslateFloat16Operation(inst);
		case IR::LoweringClass::Float: return TranslateFloatOperation(inst);
		case IR::LoweringClass::U64Mask: return TranslateU64MaskOperation(inst);
		case IR::LoweringClass::SimpleInteger: return TranslateSimpleInteger(inst);
		case IR::LoweringClass::ComposedInteger: return TranslateComposedInteger(inst);
		case IR::LoweringClass::ExtendedInteger: return TranslateExtendedInteger(inst);
	}
	return false;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail
