#pragma once

#include <cstdint>
#include <string>

namespace Libs::Graphics::ShaderRecompiler::IR {

enum class Type : uint32_t {
	Void            = 0,
	Opaque          = 1u << 0u,
	ScalarReg       = 1u << 1u,
	VectorReg       = 1u << 2u,
	U1              = 1u << 3u,
	U8              = 1u << 4u,
	U16             = 1u << 5u,
	U32             = 1u << 6u,
	U64             = 1u << 7u,
	F16             = 1u << 8u,
	F32             = 1u << 9u,
	U32x2           = 1u << 10u,
	U32x3           = 1u << 11u,
	U32x4           = 1u << 12u,
	F32x2           = 1u << 13u,
	F32x3           = 1u << 14u,
	F32x4           = 1u << 15u,
	SrtResource     = 1u << 16u,
	BufferResource  = 1u << 17u,
	AddressResource = 1u << 18u,
	ImageResource   = 1u << 19u,
	SamplerResource = 1u << 20u,
	ImageAddress    = 1u << 21u,
};

constexpr Type operator|(Type lhs, Type rhs) {
	return static_cast<Type>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr Type operator&(Type lhs, Type rhs) {
	return static_cast<Type>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

constexpr bool TypesOverlap(Type lhs, Type rhs) {
	return (lhs & rhs) != Type::Void;
}

[[nodiscard]] bool        AreTypesCompatible(Type lhs, Type rhs);
[[nodiscard]] std::string TypeName(Type type);

} // namespace Libs::Graphics::ShaderRecompiler::IR
