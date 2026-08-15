#pragma once

#include "graphics/shader/recompiler/ir/Type.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace Libs::Graphics::ShaderRecompiler::IR {

enum class Opcode {
#define IR_OPCODE(name, category, lowering, ...) name,
#include "graphics/shader/recompiler/ir/opcodes/ShaderIROpcodes.inc"
#undef IR_OPCODE
	Count,
};

enum class OpcodeClass { General, Compare, Compare64 };
enum class LoweringClass {
	Control,
	Move,
	Lane,
	State,
	Memory,
	Attribute,
	IntegerCompare,
	Integer16Compare,
	FloatCompare,
	Conversion,
	Integer16,
	PackedInteger16,
	PackedFloat16,
	Float16,
	Float,
	U64Mask,
	SimpleInteger,
	ComposedInteger,
	ExtendedInteger,
};

struct OpcodeInfo {
	std::string_view name;
	OpcodeClass      opcode_class   = OpcodeClass::General;
	LoweringClass    lowering_class = LoweringClass::Control;
};

inline constexpr OpcodeInfo OPCODE_INFO[] = {
#define IR_OPCODE(name, category, lowering, ...)                                                   \
	{#name, OpcodeClass::category, LoweringClass::lowering},
#include "graphics/shader/recompiler/ir/opcodes/ShaderIROpcodes.inc"
#undef IR_OPCODE
};
static_assert(std::size(OPCODE_INFO) == static_cast<size_t>(Opcode::Count));

constexpr const OpcodeInfo& GetOpcodeInfo(Opcode opcode) {
	return OPCODE_INFO[static_cast<size_t>(opcode)];
}

constexpr std::string_view OpcodeName(Opcode opcode) {
	return GetOpcodeInfo(opcode).name;
}

constexpr bool IsCompareOpcode(Opcode opcode) {
	return GetOpcodeInfo(opcode).opcode_class != OpcodeClass::General;
}

constexpr bool IsCompare64Opcode(Opcode opcode) {
	return GetOpcodeInfo(opcode).opcode_class == OpcodeClass::Compare64;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
