#include "graphics/shader/recompiler/frontend/translate/Translator.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

namespace {

IR::ExportTargetKind ExportTargetKindFromTarget(uint32_t target, uint32_t& index) {
	index = 0;
	switch (target) {
		case 0x08u: return IR::ExportTargetKind::MrtZ;
		case 0x09u: return IR::ExportTargetKind::Null;
		case 0x14u: return IR::ExportTargetKind::Primitive;
		default: break;
	}
	if (target <= 0x07u) {
		index = target;
		return IR::ExportTargetKind::Mrt;
	}
	if (target >= 0x0cu && target <= 0x0fu) {
		index = target - 0x0cu;
		return IR::ExportTargetKind::Position;
	}
	if (target >= 0x20u && target <= 0x3fu) {
		index = target - 0x20u;
		return IR::ExportTargetKind::Parameter;
	}
	return IR::ExportTargetKind::Unknown;
}

} // namespace

IR::ExportFlags Translator::AddExportInfo(const Decoder::Instruction& inst) {
	IR::ExportInfo info;
	info.kind        = ExportTargetKindFromTarget(inst.exp.target, info.index);
	info.target      = inst.exp.target;
	info.en          = inst.exp.en;
	info.done        = inst.exp.done;
	info.compr       = inst.exp.compr;
	info.vm          = inst.exp.vm;
	const auto index = static_cast<uint32_t>(program.export_info.size());
	program.export_info.push_back(info);
	return {.index = index, .pc = inst.pc};
}

bool Translator::TranslateEmbeddedFetch(const Decoder::Instruction& inst, uint32_t attribute,
                                        uint32_t component_count) {
	for (uint32_t component = 0; component < component_count; component++) {
		const auto value =
		    ir.Emit(IR::ValueOpcode::GetAttribute, {IR::Value(attribute), IR::Value(component)});
		WriteOperand(OffsetOperand(inst.dst, component), value);
	}
	return true;
}

void Translator::V_INTERP_P1_F32() {}

void Translator::V_INTERP_P2_F32(const Decoder::Instruction& inst) {
	const auto value = ir.Emit(IR::ValueOpcode::GetAttribute,
	                           {IR::Value(inst.src1.value), IR::Value(inst.src2.value)});
	WriteOperand(inst.dst, value);
}

bool Translator::V_INTERP_MOV_F32(const Decoder::Instruction& inst, std::string* error) {
	if (inst.src0.value >= 3u) {
		return ShaderError::Fail(error,
		                         fmt::format("v_interp_mov_f32 mode {} is reserved at pc 0x{:08x}",
		                                     inst.src0.value, inst.pc));
	}
	const auto value = ir.Emit(
	    IR::ValueOpcode::GetInterpolationParameter,
	    {IR::Value(inst.src1.value), IR::Value(inst.src2.value), IR::Value(inst.src0.value)});
	WriteOperand(inst.dst, value);
	return true;
}

bool Translator::EXP(const Decoder::Instruction& inst, std::string* error) {
	uint32_t index = 0;
	if (ExportTargetKindFromTarget(inst.exp.target, index) == IR::ExportTargetKind::Unknown) {
		return ShaderError::Fail(error,
		                         fmt::format("unsupported EXP target 0x{:02x} at pc 0x{:08x}",
		                                     inst.exp.target, inst.pc));
	}
	std::array<IR::Value, 4> components {IR::Value(0u), IR::Value(0u), IR::Value(0u),
	                                     IR::Value(0u)};
	for (uint32_t source = 0; source < std::min(inst.src_count, 4u); source++) {
		components[source] = ReadRawU32(PlainOperand(SourceAt(inst, source)));
	}
	const auto data = ir.Emit(IR::ValueOpcode::CompositeConstructU32x4,
	                          {components[0], components[1], components[2], components[3]});
	ir.Emit(IR::ValueOpcode::SetAttribute, {data, ir.GetExec()}, AddExportInfo(inst));
	return true;
}

bool Translator::EmitInterpolation(const Decoder::Instruction& inst, std::string* error) {
	switch (inst.opcode) {
		case Decoder::Opcode::V_INTERP_P1_F32: V_INTERP_P1_F32(); return true;
		case Decoder::Opcode::V_INTERP_P2_F32: V_INTERP_P2_F32(inst); return true;
		case Decoder::Opcode::V_INTERP_MOV_F32: return V_INTERP_MOV_F32(inst, error);
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
