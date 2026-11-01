#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SERIALIZE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SERIALIZE_H_

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/rectListShader.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Shader cache records. Io(io, value) appends value to a Writer or reads it back from a Reader
// field by field, so padding never reaches the bytes; instructions travel as plan-local indices.
namespace Libs::Graphics::ShaderRecompiler::IR {

struct Writer {
	std::vector<uint8_t>                      bytes;
	std::unordered_map<const Inst*, uint32_t> index; // Instructions that values may refer to.

	void Raw(const void* data, size_t size) {
		const auto* begin = static_cast<const uint8_t*>(data);
		bytes.insert(bytes.end(), begin, begin + size);
	}
};

struct Reader {
	std::span<const uint8_t> bytes;
	std::vector<Inst*>       insts;     // Instructions by index.
	bool                     ok = true; // False once a read ran past the end or was invalid.

	void Raw(void* data, size_t size) {
		ok = ok && size <= bytes.size();
		if (!ok) return;
		std::copy_n(bytes.data(), size, static_cast<uint8_t*>(data));
		bytes = bytes.subspan(size);
	}
	// Every element takes at least one byte, which bounds counts read from damaged records.
	uint32_t Count(uint32_t count) {
		ok = ok && count <= bytes.size();
		return ok ? count : 0u;
	}
};

template <class S> constexpr bool Reading = std::is_same_v<S, Reader>;
template <class T, template <class...> class Of> constexpr bool IsA = false;
template <template <class...> class Of, class... A> constexpr bool IsA<Of<A...>, Of> = true;
template <class T, class U> concept Is = std::is_same_v<std::remove_const_t<T>, U>;

template <class S, class T> void Io(S& io, T& value);

template <class S, class... T> void IoAll(S& io, T&&... values) {
	(Io(io, values), ...);
}

// The value's Type, Opaque for an instruction, then the instruction index or the zero-extended
// immediate.
inline void IoValue(Writer& writer, const Value& value) {
	auto     type = Type::Opaque;
	uint64_t bits = 0;
	if (const auto* inst = value.TryInstruction()) {
		bits = writer.index.at(inst);
	} else {
		type = value.GetType();
		switch (type) {
			case Type::Void: break;
			case Type::ScalarReg: bits = RegIndex(value.ScalarRegister()); break;
			case Type::VectorReg: bits = RegIndex(value.VectorRegister()); break;
			case Type::U1: bits = value.U1(); break;
			case Type::U8: bits = value.U8(); break;
			case Type::U16: bits = value.U16(); break;
			case Type::U32: bits = value.U32(); break;
			case Type::U64: bits = value.U64(); break;
			case Type::F16: bits = value.F16Bits(); break;
			case Type::F32: bits = std::bit_cast<uint32_t>(value.F32Value()); break;
			default: EXIT("value type %u has no cache encoding\n", static_cast<uint32_t>(type));
		}
	}
	IoAll(writer, type, bits);
}

inline void IoValue(Reader& reader, Value& value) {
	auto     type = Type::Void;
	uint64_t bits = 0;
	IoAll(reader, type, bits);
	switch (type) {
		case Type::Void: value = {}; break;
		case Type::Opaque:
			reader.ok = reader.ok && bits < reader.insts.size();
			if (reader.ok) value = Value(reader.insts[bits]);
			break;
		case Type::ScalarReg: value = Value(static_cast<ScalarReg>(bits)); break;
		case Type::VectorReg: value = Value(static_cast<VectorReg>(bits)); break;
		case Type::U1: value = Value(bits != 0u); break;
		case Type::U8: value = Value(static_cast<uint8_t>(bits)); break;
		case Type::U16: value = Value(static_cast<uint16_t>(bits)); break;
		case Type::U32: value = Value(static_cast<uint32_t>(bits)); break;
		case Type::U64: value = Value(bits); break;
		case Type::F16: value = Value::F16(static_cast<uint16_t>(bits)); break;
		case Type::F32: value = Value::F32(std::bit_cast<float>(static_cast<uint32_t>(bits))); break;
		default: reader.ok = false; break;
	}
}

// Scalars and standard containers; structs have their own overloads below.
template <class S, class T> void Io(S& io, T& value) {
	using U = std::remove_const_t<T>;
	static_assert(!std::is_pointer_v<U>, "pointers do not persist");
	if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U>) {
		io.Raw(&value, sizeof(U));
	} else if constexpr (std::is_same_v<U, Value>) {
		IoValue(io, value);
	} else if constexpr (IsA<U, std::vector> || IsA<U, std::basic_string>) {
		auto count = static_cast<uint32_t>(value.size());
		Io(io, count);
		if constexpr (Reading<S>) value.resize(io.Count(count));
		using E = typename U::value_type;
		if constexpr (std::is_arithmetic_v<E> || std::is_enum_v<E>) {
			io.Raw(value.data(), value.size() * sizeof(E));
		} else {
			for (auto& element: value) Io(io, element);
		}
	} else if constexpr (IsA<U, std::optional>) {
		bool has = value.has_value();
		Io(io, has);
		if constexpr (Reading<S>) {
			if (has) value = typename U::value_type {}; // Not emplace(): clang rejects nested aggregates.
			else value.reset();
		}
		if (has) Io(io, *value);
	} else {
		static_assert(requires { std::tuple_size<U>::value; }, "struct without an Io overload");
		std::apply([&](auto&... elements) { IoAll(io, elements...); }, value); // std::array, std::pair
	}
}

// Binds every field, so that a field added later fails to compile until it is listed.
#define KYTY_IO_FIELDS(Struct, ...)                                                                \
	template <class S, Is<Struct> T> void Io(S& io, T& object) {                                   \
		auto& [__VA_ARGS__] = object;                                                              \
		IoAll(io, __VA_ARGS__);                                                                    \
	}
KYTY_IO_FIELDS(MemoryInfo, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y)
KYTY_IO_FIELDS(BufferResource, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r)
KYTY_IO_FIELDS(ImageResource, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w)
KYTY_IO_FIELDS(SamplerResource, a, b, c, d, e, f, g, h, i, j, k)
KYTY_IO_FIELDS(SampledResourcePair, a, b, c)
KYTY_IO_FIELDS(StageInput, a, b, c, d, e)
KYTY_IO_FIELDS(StageOutput, a, b, c, d)
KYTY_IO_FIELDS(ShaderInfo, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z,
               A, B, C, D, E, F, G, H, I, J, K, L, M)
KYTY_IO_FIELDS(DescriptorSource::IndirectDescriptor::SelectorRead::CountedRows, a, b)
KYTY_IO_FIELDS(DescriptorSource::IndirectDescriptor::SelectorRead::MaskRanks, a, b, c)
KYTY_IO_FIELDS(DescriptorSource::IndirectDescriptor::SelectorRead, a, b, c, d, e, f, g, h, i, j, k,
               l)
KYTY_IO_FIELDS(DescriptorSource::IndirectDescriptor::PointerTable, a, b, c, d)
KYTY_IO_FIELDS(DescriptorSource::IndirectDescriptor, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p,
               q, r)
KYTY_IO_FIELDS(DescriptorSource, a, b, c)
KYTY_IO_FIELDS(SrtRead, a, b)
KYTY_IO_FIELDS(ResourceBlock, a, b, c, d, e)
KYTY_IO_FIELDS(UniformFill, a, b, c, d, e, f)
KYTY_IO_FIELDS(UniformFillPlan, a, b, c, d)
KYTY_IO_FIELDS(ResourceSpecialization::Buffer, a, b, c, d, e, f, g, h)
KYTY_IO_FIELDS(ResourceSpecialization::Image, a, b, c, d, e, f, g, h, i, j, k)
KYTY_IO_FIELDS(ResourceSpecialization::Sampler, a, b, c)
KYTY_IO_FIELDS(ResourceSpecialization, a, b, c)
KYTY_IO_FIELDS(Bindings, a, b, c, d, e)
KYTY_IO_FIELDS(CompiledShaderInfo, a, b, c, d, e, f, g, h, i, j)
KYTY_IO_FIELDS(Spirv::Profile, a, b, c, d)
KYTY_IO_FIELDS(ShaderTessellationInputInfo, a, b, c, d, e, f, g, h, i)
KYTY_IO_FIELDS(RectListParameter, a, b, c)
#undef KYTY_IO_FIELDS

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SERIALIZE_H_ */
