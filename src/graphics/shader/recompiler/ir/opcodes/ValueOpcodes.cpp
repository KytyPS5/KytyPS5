#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

struct OpcodeMeta {
	std::string_view     name;
	Type                 type     = Type::Void;
	std::array<Type, 16> args     = {};
	size_t               num_args = 0;
};

template <typename... Args>
consteval OpcodeMeta MakeMeta(std::string_view name, Type type, Args... args) {
	static_assert(sizeof...(Args) <= 16);
	OpcodeMeta meta {.name = name, .type = type, .num_args = sizeof...(Args)};
	meta.args.fill(Type::Void);
	size_t index = 0;
	((meta.args[index++] = args), ...);
	return meta;
}

constexpr Type Void            = Type::Void;
constexpr Type Opaque          = Type::Opaque;
constexpr Type ScalarReg       = Type::ScalarReg;
constexpr Type VectorReg       = Type::VectorReg;
constexpr Type U1              = Type::U1;
constexpr Type U8              = Type::U8;
constexpr Type U16             = Type::U16;
constexpr Type U32             = Type::U32;
constexpr Type U64             = Type::U64;
constexpr Type F16             = Type::F16;
constexpr Type F32             = Type::F32;
constexpr Type U32x2           = Type::U32x2;
constexpr Type U32x3           = Type::U32x3;
constexpr Type U32x4           = Type::U32x4;
constexpr Type F32x2           = Type::F32x2;
constexpr Type SrtResource     = Type::SrtResource;
constexpr Type BufferResource  = Type::BufferResource;
constexpr Type AddressResource = Type::AddressResource;
constexpr Type ImageResource   = Type::ImageResource;
constexpr Type SamplerResource = Type::SamplerResource;
constexpr Type LdsResource     = Type::LdsResource;
constexpr Type GdsResource     = Type::GdsResource;
constexpr Type ImageAddress    = Type::ImageAddress;

constexpr std::array<OpcodeMeta, static_cast<size_t>(ValueOpcode::Count)> MetaTable = {{
#define VALUE_OPCODE(name, ...) MakeMeta(#name __VA_OPT__(, ) __VA_ARGS__),
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.inc"
#undef VALUE_OPCODE
}};

} // namespace

Type TypeOf(ValueOpcode opcode) {
	return MetaTable[static_cast<size_t>(opcode)].type;
}

size_t NumArgsOf(ValueOpcode opcode) {
	return MetaTable[static_cast<size_t>(opcode)].num_args;
}

Type ArgTypeOf(ValueOpcode opcode, size_t index) {
	const auto& meta = MetaTable[static_cast<size_t>(opcode)];
	return meta.args.at(index);
}

std::string_view ValueOpcodeName(ValueOpcode opcode) {
	return MetaTable[static_cast<size_t>(opcode)].name;
}

bool HasSideEffects(ValueOpcode opcode) {
	const auto buffer_access = BufferAccessOf(opcode);
	if (buffer_access == BufferAccess::Write || buffer_access == BufferAccess::Atomic) {
		return true;
	}
	switch (opcode) {
		case ValueOpcode::Reference:
		case ValueOpcode::ReferenceU32:
		case ValueOpcode::StoreAddressU8:
		case ValueOpcode::StoreAddressU16:
		case ValueOpcode::StoreAddressU32:
		case ValueOpcode::WriteSharedU8:
		case ValueOpcode::WriteSharedU16:
		case ValueOpcode::WriteSharedU32:
		case ValueOpcode::WriteGdsU8:
		case ValueOpcode::WriteGdsU16:
		case ValueOpcode::WriteGdsU32:
		case ValueOpcode::SharedAtomicFMin32:
		case ValueOpcode::SharedAtomicFMax32:
		case ValueOpcode::GdsAtomicFMin32:
		case ValueOpcode::GdsAtomicFMax32:
		case ValueOpcode::SharedAtomicSwap32:
		case ValueOpcode::SharedAtomicIAdd32:
		case ValueOpcode::SharedAtomicISub32:
		case ValueOpcode::SharedAtomicSMin32:
		case ValueOpcode::SharedAtomicUMin32:
		case ValueOpcode::SharedAtomicSMax32:
		case ValueOpcode::SharedAtomicUMax32:
		case ValueOpcode::SharedAtomicAnd32:
		case ValueOpcode::SharedAtomicOr32:
		case ValueOpcode::SharedAtomicXor32:
		case ValueOpcode::GdsAtomicSwap32:
		case ValueOpcode::GdsAtomicIAdd32:
		case ValueOpcode::GdsAtomicISub32:
		case ValueOpcode::GdsAtomicSMin32:
		case ValueOpcode::GdsAtomicUMin32:
		case ValueOpcode::GdsAtomicSMax32:
		case ValueOpcode::GdsAtomicUMax32:
		case ValueOpcode::GdsAtomicAnd32:
		case ValueOpcode::GdsAtomicOr32:
		case ValueOpcode::GdsAtomicXor32:
		case ValueOpcode::ImageAtomicIAdd32:
		case ValueOpcode::ImageAtomicUMin32:
		case ValueOpcode::ImageAtomicUMax32:
		case ValueOpcode::ImageAtomicAnd32:
		case ValueOpcode::ImageAtomicOr32:
		case ValueOpcode::ImageAtomicXor32:
		case ValueOpcode::DataAppend:
		case ValueOpcode::DataConsume:
		case ValueOpcode::GdsDataAppend:
		case ValueOpcode::GdsDataConsume:
		case ValueOpcode::ImageWrite:
		case ValueOpcode::SetAttribute:
		case ValueOpcode::Barrier: return true;
		default: return false;
	}
}

BufferAccess BufferAccessOf(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::ReadConstBuffer:
		case ValueOpcode::LoadBufferU8:
		case ValueOpcode::LoadBufferU16:
		case ValueOpcode::LoadBufferU32:
		case ValueOpcode::LoadBufferU32x2:
		case ValueOpcode::LoadBufferU32x3:
		case ValueOpcode::LoadBufferU32x4: return BufferAccess::Read;
		case ValueOpcode::StoreBufferU8:
		case ValueOpcode::StoreBufferU16:
		case ValueOpcode::StoreBufferU32:
		case ValueOpcode::StoreBufferU32x2:
		case ValueOpcode::StoreBufferU32x3:
		case ValueOpcode::StoreBufferU32x4: return BufferAccess::Write;
		case ValueOpcode::BufferAtomicSwap32:
		case ValueOpcode::BufferAtomicIAdd32:
		case ValueOpcode::BufferAtomicISub32:
		case ValueOpcode::BufferAtomicSMin32:
		case ValueOpcode::BufferAtomicUMin32:
		case ValueOpcode::BufferAtomicSMax32:
		case ValueOpcode::BufferAtomicUMax32:
		case ValueOpcode::BufferAtomicAnd32:
		case ValueOpcode::BufferAtomicOr32:
		case ValueOpcode::BufferAtomicXor32:
		case ValueOpcode::BufferAtomicFMin32:
		case ValueOpcode::BufferAtomicFMax32: return BufferAccess::Atomic;
		default: return BufferAccess::None;
	}
}

uint32_t BufferComponentCount(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::LoadBufferU32x2:
		case ValueOpcode::StoreBufferU32x2: return 2u;
		case ValueOpcode::LoadBufferU32x3:
		case ValueOpcode::StoreBufferU32x3: return 3u;
		case ValueOpcode::LoadBufferU32x4:
		case ValueOpcode::StoreBufferU32x4: return 4u;
		default: return BufferAccessOf(opcode) == BufferAccess::None ? 0u : 1u;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
