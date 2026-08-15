#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail {

IR::ExportFlags Translator::AddExportInfo(const IR::Instruction& inst) {
	const auto index = static_cast<uint32_t>(value_program.export_info.size());
	value_program.export_info.push_back(inst.export_info);
	return {.index = index, .pc = inst.pc};
}

bool Translator::TranslateAttributeOperation(const IR::Instruction& inst) {
	if (inst.op == IR::Opcode::LoadInputF32) {
		WriteOperand(inst.dst,
		             ir.Emit(IR::ValueOpcode::GetAttribute,
		                     {IR::Value(inst.input_info.attr), IR::Value(inst.input_info.chan)}));
		return true;
	}
	if (inst.op == IR::Opcode::Export) {
		std::array<IR::Value, 4> components {IR::Value(0u), IR::Value(0u), IR::Value(0u),
		                                     IR::Value(0u)};
		for (uint32_t index = 0; index < std::min(inst.src_count, 4u); index++) {
			components[index] = ReadRawU32(PlainOperand(inst.src[index]));
		}
		const auto data = ir.Emit(IR::ValueOpcode::CompositeConstructU32x4,
		                          {components[0], components[1], components[2], components[3]});
		ir.Emit(IR::ValueOpcode::SetAttribute, {data, ir.GetExec()}, AddExportInfo(inst));
		return true;
	}
	return false;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail
