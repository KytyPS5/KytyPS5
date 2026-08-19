#include "graphics/shader/recompiler/ir/Type.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::IR {

bool AreTypesCompatible(Type lhs, Type rhs) {
	return lhs == rhs || lhs == Type::Opaque || rhs == Type::Opaque || TypesOverlap(lhs, rhs);
}

std::string TypeName(Type type) {
	static constexpr std::array names = {
	    "Opaque",
	    "ScalarReg",
	    "VectorReg",
	    "U1",
	    "U8",
	    "U16",
	    "U32",
	    "U64",
	    "F16",
	    "F32",
	    "U32x2",
	    "U32x3",
	    "U32x4",
	    "F32x2",
	    "F32x3",
	    "F32x4",
	    "SrtResource",
	    "BufferResource",
	    "AddressResource",
	    "ImageResource",
	    "SamplerResource",
	    "ImageAddress",
	};
	const auto bits = static_cast<uint32_t>(type);
	if (bits == 0) {
		return "Void";
	}
	std::string result;
	for (uint32_t index = 0; index < names.size(); index++) {
		if ((bits & (1u << index)) == 0) {
			continue;
		}
		if (!result.empty()) {
			result += '|';
		}
		result += names[index];
	}
	return result;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
